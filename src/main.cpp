// beansc — the beans toolchain driver
//
// usage:
//   beansc lex <file.b>...     dump the token stream
//   beansc parse <file.b>...   parse and dump the AST
//   beansc check <file.b>...   parse + type-check

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "checker.h"
#include "interp.h"
#include "lexer.h"
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

int cmd_check(const char* path) {
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
    for (const beans::ParseError& e : parser.errors()) {
        std::fprintf(stderr, "%s:%u:%u: error: %s\n", path, e.line, e.col, e.msg.c_str());
    }
    if (!lexer.errors().empty() || !parser.errors().empty()) {
        std::fprintf(stderr, "%s: stopped before type checking\n", path);
        return 1;
    }

    beans::Checker checker(mod);
    checker.run();
    for (const beans::CheckError& e : checker.errors()) {
        std::fprintf(stderr, "%s:%u:%u: error: %s\n", path, e.line, e.col, e.msg.c_str());
    }
    if (checker.errors().empty()) {
        std::printf("%s: ok\n", path);
        return 0;
    }
    std::printf("%s: %zu error(s)\n", path, checker.errors().size());
    return 1;
}

int cmd_run(const char* path) {
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
    for (const beans::ParseError& e : parser.errors()) {
        std::fprintf(stderr, "%s:%u:%u: error: %s\n", path, e.line, e.col, e.msg.c_str());
    }
    if (!lexer.errors().empty() || !parser.errors().empty()) return 1;

    beans::Checker checker(mod);
    checker.run();
    for (const beans::CheckError& e : checker.errors()) {
        std::fprintf(stderr, "%s:%u:%u: error: %s\n", path, e.line, e.col, e.msg.c_str());
    }
    if (!checker.errors().empty()) return 1;

    beans::Interp interp(mod);
    return interp.run();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <lex|parse|check|run> <file.b>...\n", argv[0]);
        return 2;
    }
    const char* cmd = argv[1];
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
