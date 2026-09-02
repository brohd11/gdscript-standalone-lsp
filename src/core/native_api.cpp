#include "core/native_api.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <unordered_set>

namespace gdscript_lsp {
namespace {

using json = nlohmann::json;

std::string value_type(const json &value) {
	if (value.contains("type")) return value.value("type", "Variant");
	if (value.contains("return_type")) return value.value("return_type", "Variant");
	if (value.contains("return_value") && value["return_value"].is_object()) {
		return value["return_value"].value("type", "Variant");
	}
	return "void";
}

std::string method_detail(const std::string &owner, const json &method) {
	std::string result = "func " + owner + "." + method.value("name", "") + "(";
	bool first = true;
	for (const auto &argument : method.value("arguments", json::array())) {
		if (!first) result += ", ";
		first = false;
		result += argument.value("name", "arg") + ": " + argument.value("type", "Variant");
	}
	return result + ") -> " + value_type(method);
}

CallableSignature callable_signature(const json &value, bool arity_known = true) {
	CallableSignature result;
	result.return_type = value_type(value);
	result.is_vararg = value.value("is_vararg", false);
	result.arity_known = arity_known;
	for (const auto &argument : value.value("arguments", json::array())) {
		result.arguments.push_back({argument.value("name", "arg"), argument.value("type", "Variant"),
			argument.contains("default_value")});
	}
	return result;
}

void add_members(NativeClass &target, const json &source, const char *key, SymbolKind kind, bool arity_known) {
	if (!source.contains(key) || !source[key].is_array()) return;
	for (const auto &entry : source[key]) {
		NativeMember member;
		member.owner = target.name;
		member.name = entry.value("name", "");
		if (member.name.empty()) continue;
		member.type = value_type(entry);
		member.kind = kind;
		member.is_static = entry.value("is_static", false);
		member.documentation = entry.value("description", "");
		if (kind == SymbolKind::Enum) {
			for (const auto &value : entry.value("values", json::array())) {
				auto name = value.value("name", "");
				if (!name.empty()) member.enum_values.insert(name);
			}
		}
		if (kind == SymbolKind::Method) {
			member.detail = method_detail(target.name, entry);
			member.signature = callable_signature(entry, arity_known);
		}
		else member.detail = member.name + ": " + member.type;
		target.members[member.name] = std::move(member);
	}
}

} // namespace

bool NativeApi::load(const std::filesystem::path &path, std::string *error) {
	classes_.clear();
	utility_functions_.clear();
	singletons_.clear();
	global_symbols_.clear();
	global_enums_.clear();
	version_.clear();
	std::ifstream stream(path);
	if (!stream) {
		if (error) *error = "cannot open " + path.string();
		return false;
	}
	json data = json::parse(stream, nullptr, false);
	if (data.is_discarded() || !data.is_object()) {
		if (error) *error = "invalid JSON in " + path.string();
		return false;
	}
	if (auto header = data.find("header"); header != data.end() && header->is_object()) {
		version_ = std::to_string(header->value("version_major", 0)) + "." +
			std::to_string(header->value("version_minor", 0)) + "." +
			std::to_string(header->value("version_patch", 0));
	}
	const bool arity_known = data.value("gdscript_lsp_schema", 0) >= 2 || data.contains("utility_functions");
	auto load_classes = [&](const char *key, bool builtin) {
		auto entries = data.find(key);
		if (entries == data.end() || !entries->is_array()) return;
		for (const auto &entry : *entries) {
			if (!entry.is_object()) continue;
			NativeClass value;
			value.name = entry.value("name", "");
			if (value.name.empty()) continue;
			value.parent = entry.value("inherits", builtin ? "" : "Object");
			value.builtin = builtin;
			for (const auto &constructor : entry.value("constructors", json::array())) {
				value.constructors.push_back(callable_signature(constructor, arity_known));
			}
			add_members(value, entry, "methods", SymbolKind::Method, arity_known);
			add_members(value, entry, "members", SymbolKind::Property, arity_known);
			add_members(value, entry, "properties", SymbolKind::Property, arity_known);
			add_members(value, entry, "signals", SymbolKind::Event, arity_known);
			add_members(value, entry, "constants", SymbolKind::Constant, arity_known);
			add_members(value, entry, "enums", SymbolKind::Enum, arity_known);
			classes_[value.name] = std::move(value);
		}
	};
	load_classes("builtin_classes", true);
	load_classes("classes", false);
	for (const auto &entry : data.value("utility_functions", json::array())) {
		auto name = entry.value("name", "");
		if (!name.empty()) utility_functions_[name] = callable_signature(entry, arity_known);
	}
	for (const auto &entry : data.value("singletons", json::array())) {
		auto name = entry.value("name", "");
		if (!name.empty()) singletons_[name] = entry.value("type", name);
	}
	for (const auto &entry : data.value("global_constants", json::array())) {
		auto name = entry.value("name", "");
		if (!name.empty()) global_symbols_.insert(name);
	}
	for (const auto &enumeration : data.value("global_enums", json::array())) {
		auto name = enumeration.value("name", "");
		if (!name.empty()) {
			global_symbols_.insert(name);
			global_enums_[name] = {};
		}
		for (const auto &value : enumeration.value("values", json::array())) {
			auto value_name = value.value("name", "");
			if (!value_name.empty()) {
				global_symbols_.insert(value_name);
				if (!name.empty()) global_enums_[name].insert(value_name);
			}
		}
	}
	return true;
}

bool NativeApi::has_class(std::string_view name) const {
	return classes_.contains(std::string(name));
}

bool NativeApi::is_builtin_class(std::string_view name) const {
	auto *record = find_class(name);
	return record && record->builtin;
}

const NativeClass *NativeApi::find_class(std::string_view name) const {
	auto found = classes_.find(std::string(name));
	return found == classes_.end() ? nullptr : &found->second;
}

const NativeMember *NativeApi::find_member(std::string_view class_name, std::string_view member) const {
	std::unordered_set<std::string> visited;
	auto current = std::string(class_name);
	while (!current.empty() && visited.insert(current).second) {
		auto *record = find_class(current);
		if (!record) return nullptr;
		auto found = record->members.find(std::string(member));
		if (found != record->members.end()) return &found->second;
		current = record->parent;
	}
	return nullptr;
}

std::vector<const NativeMember *> NativeApi::members(std::string_view class_name) const {
	std::vector<const NativeMember *> result;
	std::unordered_set<std::string> member_names;
	std::unordered_set<std::string> visited;
	auto current = std::string(class_name);
	while (!current.empty() && visited.insert(current).second) {
		auto *record = find_class(current);
		if (!record) break;
		for (const auto &[name, member] : record->members) {
			if (member_names.insert(name).second) result.push_back(&member);
		}
		current = record->parent;
	}
	return result;
}

const CallableSignature *NativeApi::find_utility_function(std::string_view name) const {
	auto found = utility_functions_.find(std::string(name));
	return found == utility_functions_.end() ? nullptr : &found->second;
}

const std::vector<CallableSignature> *NativeApi::constructors(std::string_view class_name) const {
	auto *record = find_class(class_name);
	return record ? &record->constructors : nullptr;
}

std::optional<std::string> NativeApi::singleton_type(std::string_view name) const {
	auto found = singletons_.find(std::string(name));
	if (found == singletons_.end()) return std::nullopt;
	return found->second;
}

bool NativeApi::has_global_symbol(std::string_view name) const {
	return global_symbols_.contains(std::string(name));
}

bool NativeApi::is_global_enum(std::string_view name) const {
	return global_enums_.contains(std::string(name));
}

bool NativeApi::global_enum_has_value(std::string_view enum_name, std::string_view value) const {
	auto found = global_enums_.find(std::string(enum_name));
	return found != global_enums_.end() && found->second.contains(std::string(value));
}

bool NativeApi::enum_has_value(std::string_view class_name, std::string_view enum_name, std::string_view value) const {
	auto *member = find_member(class_name, enum_name);
	return member && member->kind == SymbolKind::Enum && member->enum_values.contains(std::string(value));
}

} // namespace gdscript_lsp
