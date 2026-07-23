// The beans language server: JSON-RPC 2.0 over stdin/stdout.
//
// This file owns the transport (Content-Length framing), the lifecycle
// (initialize / shutdown / exit), and the open-document store. Feature handlers
// (diagnostics, hover, completion, ...) are added on top and reuse the compiler
// in-process via the document overlay in the loader.

#include "lsp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include "checker.h"
#include "json.h"
#include "loader.h"
#include "lsppos.h"

namespace beans {

namespace {

// file:///a/b%20c.b -> /a/b c.b  (strip scheme, percent-decode)
[[maybe_unused]] std::string uri_to_path(const std::string& uri) {
    std::string p = uri;
    const std::string scheme = "file://";
    if (p.rfind(scheme, 0) == 0) p = p.substr(scheme.size());
    std::string out;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '%' && i + 2 < p.size()) {
            auto hx = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hx(p[i + 1]), lo = hx(p[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += p[i];
    }
    return out;
}

bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// byte columns [start,end) of the identifier at (line,col), else a 1-char span
void word_cols(const std::string& text, uint32_t line, uint32_t col,
               uint32_t& start_col, uint32_t& end_col) {
    size_t p = byte_offset(text, line, col);
    if (p >= text.size() || !is_ident_char(text[p])) {
        start_col = col;
        end_col = col + 1;
        return;
    }
    size_t a = p, b = p;
    while (a > 0 && is_ident_char(text[a - 1])) a--;
    while (b < text.size() && is_ident_char(text[b])) b++;
    start_col = col - static_cast<uint32_t>(p - a);
    end_col = col + static_cast<uint32_t>(b - p);
}

Json lsp_pos_json(LspPos p) {
    Json o = Json::object();
    o.set("line", Json::number(p.line));
    o.set("character", Json::number(p.character));
    return o;
}

// an LSP diagnostic for an error at (line,col) in `text`
Json make_diag(const std::string& text, uint32_t line, uint32_t col,
               const std::string& msg) {
    uint32_t l = line ? line : 1, c = col ? col : 1;
    uint32_t sc, ec;
    word_cols(text, l, c, sc, ec);
    Json range = Json::object();
    range.set("start", lsp_pos_json(to_lsp(text, l, sc)));
    range.set("end", lsp_pos_json(to_lsp(text, l, ec)));
    Json d = Json::object();
    d.set("range", std::move(range));
    d.set("severity", Json::number(1)); // 1 = Error
    d.set("source", Json::string("beansc"));
    d.set("message", Json::string(msg));
    return d;
}

// does an error tagged for `file` belong to the document at `path`?
bool belongs(const std::string& file, const std::string& path) {
    if (file.empty() || file == path) return true;
    if (path.size() >= file.size() &&
        path.compare(path.size() - file.size(), file.size(), file) == 0)
        return true;
    return false;
}

class LspServer {
public:
    int run() {
        std::string body;
        while (read_message(body)) {
            bool ok = false;
            Json msg = json_parse(body, &ok);
            if (!ok || !msg.is_object()) continue;
            dispatch(msg);
            if (exit_now_) return exit_code_;
        }
        // stream closed without `exit`
        return shutdown_requested_ ? 0 : 1;
    }

private:
    // ---- transport --------------------------------------------------------
    bool read_message(std::string& body) {
        size_t content_length = 0;
        std::string line;
        bool saw_header = false;
        while (true) {
            int c = std::getchar();
            if (c == EOF) return false;
            if (c == '\n') {
                if (line.empty()) { // blank line ends the header block
                    if (!saw_header) continue; // tolerate stray newlines
                    break;
                }
                if (line.rfind("Content-Length:", 0) == 0)
                    content_length = std::strtoul(line.c_str() + 15, nullptr, 10);
                saw_header = true;
                line.clear();
            } else if (c != '\r') {
                line += static_cast<char>(c);
            }
        }
        body.resize(content_length);
        size_t got = content_length
                         ? std::fread(&body[0], 1, content_length, stdin)
                         : 0;
        return got == content_length;
    }

    void write_message(const Json& msg) {
        std::string s = msg.dump();
        std::printf("Content-Length: %zu\r\n\r\n", s.size());
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }

    void reply(const Json& id, Json result) {
        Json m = Json::object();
        m.set("jsonrpc", Json::string("2.0"));
        m.set("id", id);
        m.set("result", std::move(result));
        write_message(m);
    }

    void reply_error(const Json& id, int code, const std::string& message) {
        Json err = Json::object();
        err.set("code", Json::number(code));
        err.set("message", Json::string(message));
        Json m = Json::object();
        m.set("jsonrpc", Json::string("2.0"));
        m.set("id", id);
        m.set("error", std::move(err));
        write_message(m);
    }

    // ---- dispatch ---------------------------------------------------------
    void dispatch(const Json& msg) {
        std::string method = msg.get_str("method");
        const Json* id = msg.find("id");
        const Json* params = msg.find("params");
        Json empty = Json::object();
        const Json& p = params ? *params : empty;

        if (method == "initialize") { on_initialize(id, p); return; }
        if (method == "initialized") return; // notification, nothing to do
        if (method == "shutdown") {
            shutdown_requested_ = true;
            if (id) reply(*id, Json::null());
            return;
        }
        if (method == "exit") {
            exit_now_ = true;
            exit_code_ = shutdown_requested_ ? 0 : 1;
            return;
        }
        if (method == "textDocument/didOpen") { on_did_open(p); return; }
        if (method == "textDocument/didChange") { on_did_change(p); return; }
        if (method == "textDocument/didClose") { on_did_close(p); return; }
        if (method == "textDocument/hover") { on_hover(id, p); return; }

        // unknown request -> method-not-found; unknown notification -> ignore
        if (id) reply_error(*id, -32601, "method not found: " + method);
    }

    void on_initialize(const Json* id, const Json&) {
        Json caps = Json::object();
        caps.set("textDocumentSync", Json::number(1)); // 1 = full document sync
        caps.set("hoverProvider", Json::boolean(true));

        Json result = Json::object();
        result.set("capabilities", std::move(caps));
        Json info = Json::object();
        info.set("name", Json::string("beansc"));
        info.set("version", Json::string("0.1.0"));
        result.set("serverInfo", std::move(info));
        if (id) reply(*id, std::move(result));
    }

    // ---- document store ---------------------------------------------------
    void on_did_open(const Json& p) {
        const Json* doc = p.find("textDocument");
        if (!doc) return;
        std::string uri = doc->get_str("uri");
        docs_[uri].text = doc->get_str("text");
        check_and_publish(uri);
    }

    void on_did_change(const Json& p) {
        const Json* doc = p.find("textDocument");
        const Json* changes = p.find("contentChanges");
        if (!doc || !changes || !changes->is_array() || changes->arr.empty()) return;
        // full sync: the last change carries the whole document text
        std::string uri = doc->get_str("uri");
        docs_[uri].text = changes->arr.back().get_str("text");
        check_and_publish(uri);
    }

    void on_did_close(const Json& p) {
        const Json* doc = p.find("textDocument");
        if (doc) docs_.erase(doc->get_str("uri"));
    }

    // ---- checking + diagnostics -------------------------------------------
    // Load the document (with every open buffer overlaid over disk), type-check
    // it, and publish diagnostics. A successful parse is kept as the doc's
    // "last good" load so later features survive a mid-edit broken buffer.
    void check_and_publish(const std::string& uri) {
        auto it = docs_.find(uri);
        if (it == docs_.end()) return;
        const std::string& text = it->second.text;
        std::string path = uri_to_path(uri);

        std::map<std::string, std::string> overlay;
        for (const auto& [u, d] : docs_) overlay[uri_to_path(u)] = d.text;
        set_loader_overlay(&overlay);

        auto loader = std::make_shared<Loader>();
        bool loaded = loader->load(path);

        std::vector<Json> diags;
        for (const LoadError& e : loader->errors())
            if (belongs(e.file, path))
                diags.push_back(make_diag(text, e.line, e.col, e.msg));

        if (loaded) {
            Checker checker(loader->program());
            checker.run();
            for (const CheckError& e : checker.errors())
                if (belongs(e.file, path))
                    diags.push_back(make_diag(text, e.line, e.col, e.msg));
            it->second.good = loader; // keep the last good load alive
        }

        set_loader_overlay(nullptr);
        publish_diagnostics(uri, diags);
    }

    void publish_diagnostics(const std::string& uri, const std::vector<Json>& diags) {
        Json params = Json::object();
        params.set("uri", Json::string(uri));
        Json arr = Json::array();
        for (const Json& d : diags) arr.push(d);
        params.set("diagnostics", std::move(arr));
        Json m = Json::object();
        m.set("jsonrpc", Json::string("2.0"));
        m.set("method", Json::string("textDocument/publishDiagnostics"));
        m.set("params", std::move(params));
        write_message(m);
    }

    // overlay of every open buffer, keyed by filesystem path
    std::map<std::string, std::string> overlay_map() const {
        std::map<std::string, std::string> m;
        for (const auto& kv : docs_) m[uri_to_path(kv.first)] = kv.second.text;
        return m;
    }

    // ---- hover ------------------------------------------------------------
    void on_hover(const Json* id, const Json& p) {
        if (!id) return;
        const Json* td = p.find("textDocument");
        const Json* pos = p.find("position");
        auto it = td ? docs_.find(td->get_str("uri")) : docs_.end();
        if (!td || !pos || it == docs_.end()) { reply(*id, Json::null()); return; }

        const std::string& text = it->second.text;
        uint32_t line = 0, col = 0;
        from_lsp(text,
                 {static_cast<uint32_t>(pos->get_num("line")),
                  static_cast<uint32_t>(pos->get_num("character"))},
                 line, col);
        std::string path = uri_to_path(td->get_str("uri"));

        std::map<std::string, std::string> overlay = overlay_map();
        set_loader_overlay(&overlay);
        Loader loader;
        std::string md;
        if (loader.load(path)) md = hover_at(loader.program(), path, line, col);
        set_loader_overlay(nullptr);

        if (md.empty()) { reply(*id, Json::null()); return; }
        Json contents = Json::object();
        contents.set("kind", Json::string("markdown"));
        contents.set("value", Json::string(md));
        Json result = Json::object();
        result.set("contents", std::move(contents));
        reply(*id, std::move(result));
    }

    struct DocState {
        std::string text;             // current buffer contents
        std::shared_ptr<Loader> good; // last successful load (keeps its AST alive)
    };
    std::map<std::string, DocState> docs_; // uri -> state
    bool shutdown_requested_ = false;
    bool exit_now_ = false;
    int exit_code_ = 0;
};

} // namespace

int run_lsp_server() { return LspServer().run(); }

} // namespace beans
