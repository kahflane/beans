// beansc — the beans toolchain driver
//
// usage:
//   beansc lex <file.b>...     dump the token stream
//   beansc parse <file.b>...   parse and dump the AST
//   beansc check <file.b>...   parse + type-check

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "checker.h"
#include "codegen.h"
#include "interp.h"
#include "lexer.h"
#include "loader.h"
#include "lsp.h"
#include "parser.h"

namespace {

namespace fs = std::filesystem;
std::string compiler_executable;

bool read_file(const char* path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    out = buf.str();
    return true;
}

int cmd_lex(const char* path) {
    std::string source;
    if (!read_file(path, source)) {
        std::fprintf(stderr, "error: can't open %s\n", path);
        return 1;
    }
    beans::Lexer lexer(source);
    std::vector<beans::Token> tokens = lexer.scan_all();

    std::printf("== %s\n", path);
    for (const beans::Token& t : tokens) {
        if (t.kind == beans::TokenKind::newline || t.kind == beans::TokenKind::eof) {
            std::printf("%4u:%-3u %s\n", t.line, t.col, beans::to_string(t.kind));
        } else {
            std::printf("%4u:%-3u %-10s %.*s\n", t.line, t.col,
                        beans::to_string(t.kind),
                        static_cast<int>(t.text.size()), t.text.data());
        }
    }
    for (const beans::LexError& e : lexer.errors()) {
        std::fprintf(stderr, "%s:%u:%u: error: %s\n", path, e.line, e.col, e.msg.c_str());
    }
    std::printf("-- %zu tokens, %zu errors\n\n", tokens.size(), lexer.errors().size());
    return lexer.errors().empty() ? 0 : 1;
}

int cmd_parse(const char* path) {
    std::string source;
    if (!read_file(path, source)) {
        std::fprintf(stderr, "error: can't open %s\n", path);
        return 1;
    }
    beans::Lexer lexer(source);
    std::vector<beans::Token> tokens = lexer.scan_all();
    for (const beans::LexError& e : lexer.errors()) {
        std::fprintf(stderr, "%s:%u:%u: error: %s\n", path, e.line, e.col, e.msg.c_str());
    }

    beans::Parser parser(std::move(tokens));
    beans::Module mod = parser.parse_module();

    std::printf("== %s\n", path);
    std::string tree = beans::dump(mod);
    std::fwrite(tree.data(), 1, tree.size(), stdout);

    for (const beans::ParseError& e : parser.errors()) {
        std::fprintf(stderr, "%s:%u:%u: error: %s\n", path, e.line, e.col, e.msg.c_str());
    }
    size_t total_errors = lexer.errors().size() + parser.errors().size();
    std::printf("-- %zu decls, %zu errors\n\n", mod.order.size(), total_errors);
    return total_errors == 0 ? 0 : 1;
}

// load the whole program (beans.mod, packages, git imports) behind `path`.
// prints every load/parse error; returns false if anything failed.
bool load_program(const char* path, beans::Loader& loader) {
    bool ok = loader.load(path);
    for (const beans::LoadError& e : loader.errors()) {
        if (!e.file.empty() && e.line) {
            std::fprintf(stderr, "%s:%u:%u: error: %s\n", e.file.c_str(), e.line, e.col,
                         e.msg.c_str());
        } else if (!e.file.empty()) {
            std::fprintf(stderr, "%s: error: %s\n", e.file.c_str(), e.msg.c_str());
        } else {
            std::fprintf(stderr, "error: %s\n", e.msg.c_str());
        }
    }
    return ok;
}

void print_check_errors(const char* path, const beans::Checker& checker) {
    for (const beans::CheckError& e : checker.errors()) {
        const char* f = e.file.empty() ? path : e.file.c_str();
        std::fprintf(stderr, "%s:%u:%u: error: %s\n", f, e.line, e.col, e.msg.c_str());
    }
}

int cmd_check(const char* path) {
    beans::Loader loader;
    if (!load_program(path, loader)) {
        std::fprintf(stderr, "%s: stopped before type checking\n", path);
        return 1;
    }

    beans::Checker checker(loader.program());
    checker.run();
    print_check_errors(path, checker);
    if (checker.errors().empty()) {
        std::printf("%s: ok\n", path);
        return 0;
    }
    std::printf("%s: %zu error(s)\n", path, checker.errors().size());
    return 1;
}

int cmd_run(const char* path) {
    beans::Loader loader;
    if (!load_program(path, loader)) return 1;

    beans::Checker checker(loader.program());
    checker.run();
    print_check_errors(path, checker);
    if (!checker.errors().empty()) return 1;

    beans::Interp interp(checker.hir());
    return interp.run();
}

struct BuildOptions {
    bool release = false;
    bool lto = false;
    std::string cpu = "generic";
};

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += '\'';
    return quoted;
}

bool write_if_changed(const std::string& path, const std::string& contents) {
    std::string old;
    if (read_file(path.c_str(), old) && old == contents) return false;
    std::ofstream file(path, std::ios::binary);
    file << contents;
    return true;
}

unsigned long long content_hash(const std::string& contents) {
    unsigned long long hash = 14695981039346656037ULL;
    for (unsigned char byte : contents) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string find_runtime_source() {
    std::vector<fs::path> candidates;
    if (const char* override_path = std::getenv("BEANS_RUNTIME"))
        candidates.emplace_back(override_path);

    fs::path executable(compiler_executable);
    if (executable.has_parent_path()) {
        std::error_code error;
        fs::path absolute = fs::absolute(executable, error);
        if (!error) candidates.push_back(absolute.parent_path() / "beans_rt.c");
    } else if (const char* path_env = std::getenv("PATH")) {
        std::string paths(path_env);
        std::size_t start = 0;
        while (start <= paths.size()) {
            std::size_t end = paths.find(':', start);
            fs::path directory = paths.substr(start, end - start);
            if (directory.empty()) directory = ".";
            if (fs::exists(directory / executable))
                candidates.push_back(directory / "beans_rt.c");
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    // Source-tree fallbacks keep direct developer builds convenient.
    candidates.emplace_back("runtime/beans_rt.c");
    candidates.emplace_back("build/beans_rt.c");
    for (const fs::path& candidate : candidates) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error) && !error)
            return candidate.string();
    }
    return "";
}

int run_build_command(const std::string& command, const char* what) {
    int rc = std::system(command.c_str());
    if (rc != 0) std::fprintf(stderr, "error: %s failed\n", what);
    return rc;
}

int cmd_build(const char* path, const char* out_path, const BuildOptions& options) {
    beans::Loader loader;
    if (!load_program(path, loader)) return 1;

    beans::Checker checker(loader.program());
    checker.run();
    print_check_errors(path, checker);
    if (!checker.errors().empty()) return 1;

    beans::CodeGen cg(checker.hir());
    std::string ir = cg.generate();
    for (const beans::CGError& e : cg.errors()) {
        std::fprintf(stderr, "%s:%u:%u: error: %s\n", path, e.line, e.col, e.msg.c_str());
    }
    if (ir.empty()) return 1;

    // work out names: build/<stem>.ll, binary at out_path or build/<stem>
    std::string stem(path);
    size_t slash = stem.find_last_of('/');
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);

    std::system("mkdir -p build");
    std::string ll_path = "build/" + stem + ".ll";
    std::string ffi_path = "build/" + stem + "_ffi.c";
    std::string bin = out_path ? out_path : "build/" + stem;

    {
        std::ofstream f(ll_path, std::ios::binary);
        f << ir;
    }
    const std::string rt_path = find_runtime_source();
    std::string runtime_source;
    if (rt_path.empty() || !read_file(rt_path.c_str(), runtime_source)) {
        std::fprintf(stderr,
                     "error: can't find beans_rt.c beside beansc; set BEANS_RUNTIME\n");
        return 1;
    }
    if (!cg.ffi_c().empty()) write_if_changed(ffi_path, cg.ffi_c());
    const std::string optimize = options.release ? "-O3 -DNDEBUG" : "-O2";
    const std::string cpu = options.cpu == "native" ? " -march=native" : "";
    const std::string lto = options.lto ? " -flto" : "";
    const std::string flavor = std::string(options.release ? ".release" : ".dev") +
                               (options.lto ? ".lto" : "") +
                               (options.cpu == "native" ? ".native" : "");
    const std::string runtime_artifact =
        "build/beans_rt" + flavor + "." + std::to_string(content_hash(runtime_source)) +
        (options.lto ? ".bc" : ".o");

    std::ifstream cached(runtime_artifact, std::ios::binary);
    if (!cached.good()) {
        std::string runtime_cmd = "clang " + optimize + lto + cpu + " -pthread";
        if (options.lto) runtime_cmd += " -emit-llvm";
        runtime_cmd += " -c " + shell_quote(rt_path) + " -o " +
                       shell_quote(runtime_artifact);
        if (run_build_command(runtime_cmd, "clang runtime compile") != 0) return 1;
    }

    std::string cmd = "clang " + optimize + lto + cpu + " -pthread" +
                      " -Wno-override-module " + shell_quote(ll_path) + " " +
                      shell_quote(runtime_artifact);
    if (!cg.ffi_c().empty()) cmd += " " + shell_quote(ffi_path);
    cmd += " -lm -o " + shell_quote(bin);
    int rc = run_build_command(cmd, "clang link");
    if (rc != 0) {
        std::fprintf(stderr, "error: generated IR is in %s\n", ll_path.c_str());
        return 1;
    }
    std::printf("built %s\n", bin.c_str());
    return 0;
}

// lsp-probe <file.b>:<line>:<col> — print the Markdown a hover would show at
// that position. A debug window into the language server's hover, testable from
// the shell before any editor is wired up.
int cmd_lsp_probe(const char* arg) {
    std::string spec(arg);
    // split from the right so file paths may contain colons
    size_t c2 = spec.rfind(':');
    size_t c1 = (c2 == std::string::npos || c2 == 0)
                    ? std::string::npos
                    : spec.rfind(':', c2 - 1);
    if (c1 == std::string::npos || c2 == std::string::npos) {
        std::fprintf(stderr, "usage: %s lsp-probe <file.b>:<line>:<col>\n",
                     compiler_executable.c_str());
        return 2;
    }
    std::string file = spec.substr(0, c1);
    long line = std::strtol(spec.c_str() + c1 + 1, nullptr, 10);
    long col = std::strtol(spec.c_str() + c2 + 1, nullptr, 10);
    if (line <= 0 || col <= 0) {
        std::fprintf(stderr, "lsp-probe: line and col are 1-based positive integers\n");
        return 2;
    }

    beans::Loader loader;
    if (!load_program(file.c_str(), loader)) {
        std::fprintf(stderr, "%s: stopped before hover\n", file.c_str());
        return 1;
    }
    std::string md = beans::hover_at(loader.program(), file,
                                     static_cast<uint32_t>(line),
                                     static_cast<uint32_t>(col));
    if (md.empty()) {
        std::fprintf(stderr, "no symbol at %s:%ld:%ld\n", file.c_str(), line, col);
        return 1;
    }
    std::printf("%s\n", md.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    compiler_executable = argc > 0 ? argv[0] : "beansc";
    // the language server takes no file arguments — it speaks JSON-RPC on stdio
    if (argc >= 2 && std::strcmp(argv[1], "lsp") == 0) {
        return beans::run_lsp_server();
    }
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <lex|parse|check|run> <file.b>...\n"
                     "       %s build [--release] [--lto] [--cpu generic|native] "
                     "<file.b> [-o out]\n"
                     "       %s lsp-probe <file.b>:<line>:<col>\n"
                     "       %s lsp   (language server on stdio)\n",
                     argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    const char* cmd = argv[1];
    if (std::strcmp(cmd, "build") == 0) {
        const char* out = nullptr;
        const char* file = nullptr;
        BuildOptions options;
        for (int i = 2; i < argc; i++) {
            if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (std::strcmp(argv[i], "--release") == 0) {
                options.release = true;
            } else if (std::strcmp(argv[i], "--lto") == 0) {
                options.lto = true;
            } else if (std::strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
                options.cpu = argv[++i];
                if (options.cpu != "generic" && options.cpu != "native") {
                    std::fprintf(stderr, "error: --cpu must be generic or native\n");
                    return 2;
                }
            } else if (argv[i][0] == '-') {
                std::fprintf(stderr, "error: unknown build option: %s\n", argv[i]);
                return 2;
            } else if (file) {
                std::fprintf(stderr, "error: build accepts one entry file\n");
                return 2;
            } else {
                file = argv[i];
            }
        }
        if (!file) {
            std::fprintf(stderr, "usage: %s build <file.b> [-o out]\n", argv[0]);
            return 2;
        }
        return cmd_build(file, out, options);
    }

    if (std::strcmp(cmd, "lsp-probe") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: %s lsp-probe <file.b>:<line>:<col>\n", argv[0]);
            return 2;
        }
        return cmd_lsp_probe(argv[2]);
    }

    // `run f.b -- a b` hands everything after -- to the program (os.args)
    int stop = argc;
    if (std::strcmp(cmd, "run") == 0) {
        for (int i = 2; i < argc; i++) {
            if (std::strcmp(argv[i], "--") == 0) {
                beans::set_program_args(
                    std::vector<std::string>(argv + i + 1, argv + argc));
                stop = i;
                break;
            }
        }
    }

    int rc = 0;
    for (int i = 2; i < stop; i++) {
        if (std::strcmp(cmd, "lex") == 0) rc |= cmd_lex(argv[i]);
        else if (std::strcmp(cmd, "parse") == 0) rc |= cmd_parse(argv[i]);
        else if (std::strcmp(cmd, "check") == 0) rc |= cmd_check(argv[i]);
        else if (std::strcmp(cmd, "run") == 0) rc |= cmd_run(argv[i]);
        else {
            std::fprintf(stderr, "unknown command: %s\n", cmd);
            return 2;
        }
    }
    return rc;
}
