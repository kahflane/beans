#include "loader.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "lexer.h"
#include "parser.h"

namespace fs = std::filesystem;

namespace beans {

namespace {

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    out = buf.str();
    return true;
}

std::string last_segment(const std::string& path) {
    size_t cut = path.find_last_of("./");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

std::string beans_home() {
    if (const char* h = std::getenv("BEANS_HOME")) return h;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.beans";
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

} // namespace

void Loader::error(std::string file, uint32_t line, uint32_t col, std::string msg) {
    errors_.push_back({std::move(file), std::move(msg), line, col});
}

// ---- beans.mod --------------------------------------------------------------

bool Loader::read_beans_mod(const std::string& dir, std::string& module_name,
                            const std::string& from_file, uint32_t line, uint32_t col) {
    std::string text;
    if (!read_file(dir + "/beans.mod", text)) {
        error(from_file, line, col, "no beans.mod in " + dir);
        return false;
    }
    module_name.clear();
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        std::istringstream ls(raw);
        std::string word;
        if (!(ls >> word) || word.rfind("//", 0) == 0) continue;
        if (word == "module") {
            ls >> module_name;
        } else if (word == "require") {
            std::string path, tag;
            ls >> path >> tag;
            if (!path.empty() && !tag.empty() && !pins_.count(path)) pins_[path] = tag;
        } else {
            error(dir + "/beans.mod", 0, 0, "unknown beans.mod line: " + raw);
        }
    }
    if (module_name.empty()) {
        error(dir + "/beans.mod", 0, 0, "beans.mod needs a 'module <name>' line");
        return false;
    }
    return true;
}

// ---- git fetch --------------------------------------------------------------

std::string Loader::ensure_remote(const std::string& host_path, const std::string& from_file,
                                  uint32_t line, uint32_t col) {
    // host_path is host/owner/repo — checkout lands in $BEANS_HOME/src/host/owner/repo
    std::string dir = beans_home() + "/src/" + host_path;
    if (fs::exists(dir + "/beans.mod")) return dir;
    if (fs::exists(dir)) {
        error(from_file, line, col, "cached package " + dir + " has no beans.mod");
        return "";
    }

    std::error_code ec;
    fs::create_directories(fs::path(dir).parent_path(), ec);

    std::string url = "https://" + host_path + ".git";
    std::string cmd = "git clone --quiet --depth 1 ";
    auto pin = pins_.find(host_path);
    if (pin != pins_.end()) cmd += "--branch " + shell_quote(pin->second) + " ";
    cmd += shell_quote(url) + " " + shell_quote(dir);

    if (std::system(cmd.c_str()) != 0) {
        fs::remove_all(dir, ec); // no half-fetched cache entries
        error(from_file, line, col,
              "couldn't fetch " + host_path + " (git clone " + url + " failed)");
        return "";
    }
    if (!fs::exists(dir + "/beans.mod")) {
        error(from_file, line, col, host_path + " isn't a beans package (no beans.mod)");
        return "";
    }
    return dir;
}

// ---- packages ---------------------------------------------------------------

bool Loader::parse_file(Package& pkg, const std::string& path) {
    auto pf = std::make_unique<PFile>();
    pf->path = path;
    if (!read_file(path, pf->source)) {
        error(path, 0, 0, "can't open file");
        return false;
    }

    Lexer lexer(pf->source);
    std::vector<Token> tokens = lexer.scan_all();
    for (const LexError& e : lexer.errors()) error(path, e.line, e.col, e.msg);

    Parser parser(std::move(tokens));
    pf->mod = parser.parse_module();
    for (const ParseError& e : parser.errors()) error(path, e.line, e.col, e.msg);

    // stamp package-qualified names; root package ("" prefix) keeps plain names
    if (!pkg.prefix.empty()) {
        for (ClassDecl& c : pf->mod.classes) c.qualname = pkg.prefix + "." + c.name;
        for (EnumDecl& e : pf->mod.enums) e.qualname = pkg.prefix + "." + e.name;
        for (FnDecl& f : pf->mod.fns) f.qualname = pkg.prefix + "." + f.name;
    }

    pkg.files.push_back(std::move(pf));
    return true;
}

void Loader::resolve_imports(Package& pkg, const ModuleCtx& ctx) {
    for (const auto& pf : pkg.files) {
        for (const ImportDecl& imp : pf->mod.imports) {
            const std::string& p = imp.path;
            if (p.rfind("std.", 0) == 0) continue; // builtins, no package behind them

            // local: shop.util -> <root>/util/
            if (!ctx.name.empty() &&
                (p == ctx.name || p.rfind(ctx.name + ".", 0) == 0)) {
                if (p == ctx.name) {
                    error(pf->path, imp.line, imp.col, "a package can't import its own module root");
                    continue;
                }
                std::string sub = p.substr(ctx.name.size() + 1);
                std::string dir = ctx.root;
                for (char& c : sub) {
                    if (c == '.') c = '/';
                }
                dir += "/" + sub;
                load_package(p, dir, last_segment(p), ctx, pf->path, imp.line, imp.col);
                continue;
            }

            // remote: github.com/acme/http[/sub] — first segment has a dot
            size_t first_sep = p.find_first_of("./");
            bool hosty = first_sep != std::string::npos && p[first_sep] == '.' &&
                         p.find('/') != std::string::npos;
            if (hosty) {
                // host/owner/repo is the clone unit; anything after is a subdir
                std::vector<std::string> segs;
                std::istringstream ss(p);
                std::string seg;
                while (std::getline(ss, seg, '/')) segs.push_back(seg);
                if (segs.size() < 3) {
                    error(pf->path, imp.line, imp.col,
                          "git import needs host/owner/repo, got '" + p + "'");
                    continue;
                }
                std::string repo = segs[0] + "/" + segs[1] + "/" + segs[2];
                std::string checkout = ensure_remote(repo, pf->path, imp.line, imp.col);
                if (checkout.empty()) continue;

                auto ctx_it = remote_ctx_.find(checkout);
                if (ctx_it == remote_ctx_.end()) {
                    ModuleCtx rc;
                    rc.root = checkout;
                    if (!read_beans_mod(checkout, rc.name, pf->path, imp.line, imp.col)) continue;
                    ctx_it = remote_ctx_.emplace(checkout, std::move(rc)).first;
                }
                std::string dir = checkout;
                for (size_t i = 3; i < segs.size(); i++) dir += "/" + segs[i];
                load_package(p, dir, last_segment(p), ctx_it->second,
                             pf->path, imp.line, imp.col);
                continue;
            }

            if (ctx.name.empty()) {
                error(pf->path, imp.line, imp.col,
                      "unknown package '" + p +
                          "' — a single file can import std.* or a git host path; "
                          "local packages need a beans.mod");
            } else {
                error(pf->path, imp.line, imp.col,
                      "unknown package '" + p + "' — expected std.*, " + ctx.name +
                          ".*, or a git host path");
            }
        }
    }
}

std::string Loader::load_package(const std::string& import_path, const std::string& dir,
                                 const std::string& prefix, const ModuleCtx& ctx,
                                 const std::string& from_file, uint32_t line, uint32_t col) {
    auto st = state_.find(import_path);
    if (st != state_.end()) {
        if (st->second == 1) {
            error(from_file, line, col, "import cycle through '" + import_path + "'");
            return "";
        }
        return import_path; // already loaded
    }

    // one short name per program — decls are qualified by it
    auto pre = prefix_of_.find(prefix);
    if (pre != prefix_of_.end() && pre->second != import_path) {
        error(from_file, line, col, "two packages both named '" + prefix + "' (" +
                                        pre->second + " and " + import_path +
                                        ") — rename one directory");
        return "";
    }
    prefix_of_[prefix] = import_path;
    state_[import_path] = 1;

    auto pkg = std::make_unique<Package>();
    pkg->prefix = prefix;
    pkg->import_path = import_path;
    pkg->dir = dir;

    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".b") files.push_back(entry.path().string());
    }
    if (ec) {
        error(from_file, line, col, "package directory " + dir + " doesn't exist");
        state_[import_path] = 2;
        return "";
    }
    if (files.empty()) {
        error(from_file, line, col, "package " + import_path + " has no .b files in " + dir);
        state_[import_path] = 2;
        return "";
    }
    std::sort(files.begin(), files.end());
    for (const std::string& f : files) parse_file(*pkg, f);

    resolve_imports(*pkg, ctx); // recursion lands dependencies first
    prog_.packages.push_back(std::move(pkg));
    state_[import_path] = 2;
    return import_path;
}

// ---- entry ------------------------------------------------------------------

bool Loader::load(const std::string& entry) {
    if (!fs::exists(entry)) {
        error(entry, 0, 0, "can't open file");
        return false;
    }
    fs::path dir = fs::absolute(entry).parent_path();

    // walk up looking for beans.mod
    std::string mod_root;
    for (fs::path d = dir; !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / "beans.mod")) {
            mod_root = d.string();
            break;
        }
        if (d == d.root_path()) break;
    }

    if (mod_root.empty()) {
        // single-file mode
        auto pkg = std::make_unique<Package>();
        pkg->prefix = "";
        pkg->import_path = "main";
        pkg->dir = dir.string();
        if (!parse_file(*pkg, entry)) return false;
        ModuleCtx ctx; // empty name = no local imports allowed
        resolve_imports(*pkg, ctx);
        prog_.packages.push_back(std::move(pkg));
        return errors_.empty();
    }

    ModuleCtx ctx;
    ctx.root = mod_root;
    if (!read_beans_mod(mod_root, ctx.name, entry, 0, 0)) return false;
    prog_.module_name = ctx.name;

    if (dir.string() != mod_root) {
        error(entry, 0, 0, "entry file must sit next to beans.mod (in " + mod_root + ")");
        return false;
    }

    prefix_of_[""] = ctx.name;
    state_[ctx.name] = 1;

    auto pkg = std::make_unique<Package>();
    pkg->prefix = "";
    pkg->import_path = ctx.name;
    pkg->dir = mod_root;

    std::vector<std::string> files;
    for (const auto& e : fs::directory_iterator(mod_root)) {
        if (e.path().extension() == ".b") files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    for (const std::string& f : files) parse_file(*pkg, f);

    resolve_imports(*pkg, ctx);
    prog_.packages.push_back(std::move(pkg));
    state_[ctx.name] = 2;
    return errors_.empty();
}

} // namespace beans
