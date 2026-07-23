#include "c_abi.h"

#include <map>
#include <set>
#include <utility>

namespace beans {
namespace {

class CTextBuilder {
public:
    CTextBuilder(const CAbiRecordLookup& lookup, std::string prefix)
        : lookup_(lookup), prefix_(std::move(prefix)) {}

    CAbiText describe(const FnDecl& function) {
        CAbiText result;
        result.return_type = function.ret ? base(function.ret.get()) : "void";
        for (size_t i = 0; i < function.params.size(); i++) {
            const TypeRef* type = function.params[i].type.get();
            if (type && type->kind == TypeRef::Kind::fn) {
                CAbiCallbackText callback;
                callback.parameter_index = i;
                callback.return_type = type->fn_ret ? base(type->fn_ret.get()) : "void";
                std::string declaration = callback.return_type + " (*arg" +
                                          std::to_string(i) + ")(";
                for (size_t pi = 0; pi < type->fn_params.size(); pi++) {
                    if (pi) declaration += ", ";
                    callback.parameter_types.push_back(base(type->fn_params[pi].get()));
                    callback.parameter_declarations.push_back(
                        this->declaration(type->fn_params[pi].get(),
                                          "value" + std::to_string(pi)));
                    declaration += callback.parameter_declarations.back();
                }
                if (type->fn_params.empty()) declaration += "void";
                declaration += ")";
                result.parameter_types.push_back("void*");
                result.parameter_declarations.push_back(std::move(declaration));
                result.callbacks.push_back(std::move(callback));
            } else {
                result.parameter_types.push_back(base(type));
                result.parameter_declarations.push_back(
                    declaration(type, "arg" + std::to_string(i)));
            }
        }
        result.definitions = definitions_;
        return result;
    }

private:
    std::string record(const ClassDecl* declaration) {
        auto named = names_.find(declaration);
        if (named == names_.end()) {
            std::string name = prefix_ + "Record" + std::to_string(names_.size());
            named = names_.emplace(declaration, std::move(name)).first;
        }
        if (emitted_.count(declaration)) return named->second;
        if (!active_.insert(declaration).second) return named->second;
        std::string fields;
        for (size_t i = 0; i < declaration->fields.size(); i++) {
            fields += "  " +
                      this->declaration(declaration->fields[i].type.get(),
                                        "field" + std::to_string(i)) +
                      ";\n";
        }
        active_.erase(declaration);
        definitions_ +=
            "typedef " +
            std::string(declaration->is_union ? "union" : "struct") + " {\n" +
            fields + "} " + named->second + ";\n";
        emitted_.insert(declaration);
        return named->second;
    }

    std::string base(const TypeRef* type) {
        if (!type) return "void";
        if (type->kind == TypeRef::Kind::fixed_array)
            return base(type->array_elem.get());
        if (type->kind != TypeRef::Kind::named) return "void";
        const std::string& name = type->name;
        if (name == "i8") return "int8_t";
        if (name == "u8" || name == "byte") return "uint8_t";
        if (name == "i16") return "int16_t";
        if (name == "u16") return "uint16_t";
        if (name == "i32") return "int32_t";
        if (name == "u32") return "uint32_t";
        if (name == "i64" || name == "int") return "int64_t";
        if (name == "u64") return "uint64_t";
        if (name == "f32") return "float";
        if (name == "f64" || name == "float") return "double";
        if (name == "bool") return "_Bool";
        if (name == "RawPtr") return "void*";
        if (const ClassDecl* value = lookup_(type)) return record(value);
        return "void";
    }

    std::string declaration(const TypeRef* type, const std::string& name) {
        const TypeRef* element = type;
        std::vector<uint32_t> dimensions;
        while (element && element->kind == TypeRef::Kind::fixed_array) {
            dimensions.push_back(element->array_len);
            element = element->array_elem.get();
        }
        std::string result = base(element) + " " + name;
        for (uint32_t dimension : dimensions)
            result += "[" + std::to_string(dimension) + "]";
        return result;
    }

    const CAbiRecordLookup& lookup_;
    std::string prefix_;
    std::map<const ClassDecl*, std::string> names_;
    std::set<const ClassDecl*> emitted_;
    std::set<const ClassDecl*> active_;
    std::string definitions_;
};

} // namespace

CAbiText describe_c_abi(const FnDecl& function,
                        const CAbiRecordLookup& lookup,
                        std::string record_prefix) {
    return CTextBuilder(lookup, std::move(record_prefix)).describe(function);
}

} // namespace beans
