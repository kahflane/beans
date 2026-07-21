// beansc — the beans toolchain driver
//
// usage:
//   beansc lex <file.b>...     dump the token stream
//   beansc parse <file.b>...   parse and dump the AST
//   beansc check <file.b>...   parse + type-check

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "checker.h"
#include "codegen.h"
#include "interp.h"
#include "lexer.h"
#include "loader.h"
#include "parser.h"

namespace {

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

    beans::Interp interp(loader.program());
    return interp.run();
}

int cmd_build(const char* path, const char* out_path) {
    beans::Loader loader;
    if (!load_program(path, loader)) return 1;

    beans::Checker checker(loader.program());
    checker.run();
    print_check_errors(path, checker);
    if (!checker.errors().empty()) return 1;

    beans::CodeGen cg(loader.program());
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
    std::string rt_path = "build/beans_rt.c";
    std::string bin = out_path ? out_path : "build/" + stem;

    {
        std::ofstream f(ll_path, std::ios::binary);
        f << ir;
    }
    {
        std::ofstream f(rt_path, std::ios::binary);
        f << beans::CodeGen::runtime_c();
    }

    std::string cmd = "clang -O2 -Wno-override-module " + ll_path + " " + rt_path +
                      " -o " + bin;
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "error: clang failed on the generated IR (%s)\n",
                     ll_path.c_str());
        return 1;
    }
    std::printf("built %s\n", bin.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <lex|parse|check|run> <file.b>...\n"
                     "       %s build <file.b> [-o out]\n",
                     argv[0], argv[0]);
        return 2;
    }
    const char* cmd = argv[1];
    if (std::strcmp(cmd, "build") == 0) {
        const char* out = nullptr;
        const char* file = nullptr;
        for (int i = 2; i < argc; i++) {
            if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else {
                file = argv[i];
            }
        }
        if (!file) {
            std::fprintf(stderr, "usage: %s build <file.b> [-o out]\n", argv[0]);
            return 2;
        }
        return cmd_build(file, out);
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
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
