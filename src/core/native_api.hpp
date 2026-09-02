#pragma once

#include "core/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gdscript_lsp {

struct NativeArgument {
	std::string name;
	std::string type = "Variant";
	bool has_default = false;
};

struct CallableSignature {
	std::string return_type = "void";
	std::vector<NativeArgument> arguments;
	bool is_vararg = false;
	bool arity_known = true;
};

struct NativeMember {
	std::string owner;
	std::string name;
	std::string type;
	std::string detail;
	std::string documentation;
	SymbolKind kind = SymbolKind::Property;
	bool is_static = false;
	std::optional<CallableSignature> signature;
	std::unordered_set<std::string> enum_values;
};

struct NativeClass {
	std::string name;
	std::string parent;
	bool builtin = false;
	std::unordered_map<std::string, NativeMember> members;
	std::vector<CallableSignature> constructors;
};

class NativeApi {
public:
	bool load(const std::filesystem::path &path, std::string *error = nullptr);
	bool has_class(std::string_view name) const;
	bool is_builtin_class(std::string_view name) const;
	const NativeClass *find_class(std::string_view name) const;
	const NativeMember *find_member(std::string_view class_name, std::string_view member) const;
	std::vector<const NativeMember *> members(std::string_view class_name) const;
	const CallableSignature *find_utility_function(std::string_view name) const;
	const std::vector<CallableSignature> *constructors(std::string_view class_name) const;
	std::optional<std::string> singleton_type(std::string_view name) const;
	bool has_global_symbol(std::string_view name) const;
	bool is_global_enum(std::string_view name) const;
	std::optional<std::string> global_enum_for_value(std::string_view value) const;
	bool global_enum_has_value(std::string_view enum_name, std::string_view value) const;
	bool has_enum(std::string_view class_name, std::string_view enum_name) const;
	bool enum_has_value(std::string_view class_name, std::string_view enum_name, std::string_view value) const;
	const std::unordered_map<std::string, NativeClass> &classes() const { return classes_; }
	std::string version() const { return version_; }

private:
	std::unordered_map<std::string, NativeClass> classes_;
	std::unordered_map<std::string, CallableSignature> utility_functions_;
	std::unordered_map<std::string, std::string> singletons_;
	std::unordered_set<std::string> global_symbols_;
	std::unordered_map<std::string, std::unordered_set<std::string>> global_enums_;
	std::unordered_map<std::string, std::string> global_enum_values_;
	std::string version_;
};

} // namespace gdscript_lsp
