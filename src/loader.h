#pragma once

#include <map>
#include <string>
#include <vector>

#include "ast.h"

namespace beans {

struct LoadError {
    std::string file; // empty = module-level problem (bad beans.mod, fetch failure)
    std::string msg;
    uint32_t line = 0, col = 0;
};

// Turns one entry file into a whole Program.
//
// With no beans.mod above the entry file: single-file mode, one package,
// std imports only — exactly the pre-module behaviour.
//
// With beans.mod (`module shop`): the entry file's directory is the root
// package (every .b file in it), `import shop.util` pulls root/util/, and
// `import github.com/acme/http` clones the repo into $BEANS_HOME/src (default
// ~/.beans/src) on first use. `require <path> <tag>` lines pin git tags.
//
// Every declaration is stamped with its package-qualified name ("util.User");
// root-package decls keep their plain name, so single files are unchanged.
class Loader {
public:
    bool load(const std::string& entry); // false = errors() has the reasons
    Program& program() { return prog_; }
    const std::vector<LoadError>& errors() const { return errors_; }

private:
    struct ModuleCtx {
        std::string name;  // module name from beans.mod
        std::string root;  // directory that holds beans.mod
        std::string canon; // git modules: host/owner/repo — canonical base for
                           // their internal imports; empty for the app module
    };

    // returns the package's prefix-qualified key, or "" on error
    std::string load_package(const std::string& import_path, const std::string& dir,
                             const std::string& prefix, const ModuleCtx& ctx,
                             const std::string& from_file, uint32_t line, uint32_t col);
    bool parse_file(Package& pkg, const std::string& path);
    void resolve_imports(Package& pkg, const ModuleCtx& ctx);

    bool read_beans_mod(const std::string& dir, std::string& module_name,
                        const std::string& from_file, uint32_t line, uint32_t col);
    std::string ensure_remote(const std::string& host_path, const std::string& from_file,
                              uint32_t line, uint32_t col); // returns checkout dir or ""

    void error(std::string file, uint32_t line, uint32_t col, std::string msg);

    Program prog_;
    std::vector<LoadError> errors_;

    std::map<std::string, int> state_;            // import path -> 0 new / 1 loading / 2 done
    std::map<std::string, std::string> prefix_of_; // prefix -> import path (clash detection)
    std::map<std::string, std::string> pins_;      // repo path -> git tag (require lines)
    std::map<std::string, ModuleCtx> remote_ctx_;  // checkout root -> its module ctx
};

} // namespace beans
