#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ast.h"

namespace beans {

struct CAbiCallbackText {
    size_t parameter_index = 0;
    std::string return_type;
    std::vector<std::string> parameter_types;
    std::vector<std::string> parameter_declarations;
};

struct CAbiText {
    std::string definitions;
    std::string return_type;
    std::vector<std::string> parameter_types;
    std::vector<std::string> parameter_declarations;
    std::vector<CAbiCallbackText> callbacks;
};

using CAbiRecordLookup =
    std::function<const ClassDecl*(const TypeRef*)>;

// Recreate a checked @c_layout signature as C source. Clang then owns the
// target-specific aggregate ABI classification in both native wrappers and
// interpreter trampolines.
CAbiText describe_c_abi(const FnDecl& function,
                        const CAbiRecordLookup& lookup,
                        std::string record_prefix = "BeansFfi");

} // namespace beans
