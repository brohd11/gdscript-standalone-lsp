#include "core/workspace.hpp"
#include "core/semantic_analyzer.hpp"
#include "core/text.hpp"
#include "core/uri.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>

namespace gdscript_lsp {
namespace {

std::string read_file(const std::filesystem::path &path) {
	std::ifstream stream(path, std::ios::binary);
	if (!stream) return {};
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool is_identifier(std::string_view value) {
	if (value.empty() || (!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_')) return false;
	return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

bool is_integer_literal(std::string_view value) {
	if (!value.empty() && (value.front() == '+' || value.front() == '-')) value.remove_prefix(1);
	return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool is_float_literal(std::string_view value) {
	if (!value.empty() && (value.front() == '+' || value.front() == '-')) value.remove_prefix(1);
	auto dot = value.find('.');
	if (dot == std::string_view::npos || dot + 1 == value.size()) return false;
	auto digits = [](std::string_view part) {
		return std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); });
	};
	return digits(value.substr(0, dot)) && digits(value.substr(dot + 1));
}

std::optional<std::pair<std::string, std::string>> member_call(std::string_view value) {
	auto dot = value.find('.');
	if (dot == std::string_view::npos) return std::nullopt;
	auto receiver = value.substr(0, dot);
	if (!is_identifier(receiver)) return std::nullopt;
	size_t end = dot + 1;
	while (end < value.size() && (std::isalnum(static_cast<unsigned char>(value[end])) || value[end] == '_')) ++end;
	auto member = value.substr(dot + 1, end - dot - 1);
	if (!is_identifier(member)) return std::nullopt;
	while (end < value.size() && std::isspace(static_cast<unsigned char>(value[end]))) ++end;
	if (end >= value.size() || value[end] != '(') return std::nullopt;
	return std::pair{std::string(receiver), std::string(member)};
}

std::optional<std::string> function_call(std::string_view value) {
	size_t end = 0;
	while (end < value.size() && (std::isalnum(static_cast<unsigned char>(value[end])) || value[end] == '_')) ++end;
	auto function = value.substr(0, end);
	if (!is_identifier(function)) return std::nullopt;
	while (end < value.size() && std::isspace(static_cast<unsigned char>(value[end]))) ++end;
	if (end >= value.size() || value[end] != '(') return std::nullopt;
	return std::string(function);
}

std::optional<std::string> completion_receiver(std::string_view prefix) {
	auto dot = prefix.rfind('.');
	if (dot == std::string_view::npos) return std::nullopt;
	for (size_t i = dot + 1; i < prefix.size(); ++i) {
		if (!std::isalnum(static_cast<unsigned char>(prefix[i])) && prefix[i] != '_') return std::nullopt;
	}
	size_t start = dot;
	while (start > 0 && (std::isalnum(static_cast<unsigned char>(prefix[start - 1])) || prefix[start - 1] == '_')) --start;
	auto receiver = prefix.substr(start, dot - start);
	if (!is_identifier(receiver)) return std::nullopt;
	return std::string(receiver);
}

std::string normalize_api_type(std::string value) {
	if (value.starts_with("typedarray::")) return "Array[" + value.substr(12) + "]";
	if (value.starts_with("enum::")) return value.substr(6);
	if (value.starts_with("bitfield::")) return value.substr(10);
	return value;
}

// Mirrors the useful type-level portion of Godot's Variant::can_convert_strict.
// These are the implicit conversions accepted by the GDScript analyzer, not the
// broader set of runtime Variant conversions.
bool can_convert_strict(std::string_view source, std::string_view target) {
	if (source == target) return true;
	if ((target == "bool" || target == "int" || target == "float") &&
			(source == "bool" || source == "int" || source == "float")) return true;
	if (target == "String" && (source == "StringName" || source == "NodePath")) return true;
	if (target == "StringName" && source == "String") return true;
	if (target == "NodePath" && source == "String") return true;
	if ((target == "Vector2" && source == "Vector2i") || (target == "Vector2i" && source == "Vector2") ||
			(target == "Rect2" && source == "Rect2i") || (target == "Rect2i" && source == "Rect2") ||
			(target == "Vector3" && source == "Vector3i") || (target == "Vector3i" && source == "Vector3") ||
			(target == "Vector4" && source == "Vector4i") || (target == "Vector4i" && source == "Vector4")) return true;
	if ((target == "Quaternion" && source == "Basis") || (target == "Basis" && source == "Quaternion") ||
			(target == "Transform2D" && source == "Transform3D") ||
			(target == "Transform3D" && (source == "Transform2D" || source == "Quaternion" || source == "Basis" || source == "Projection")) ||
			(target == "Projection" && source == "Transform3D")) return true;
	if (target == "Color" && (source == "String" || source == "int")) return true;
	static const std::unordered_set<std::string_view> packed_arrays = {
		"PackedByteArray", "PackedInt32Array", "PackedInt64Array", "PackedFloat32Array",
		"PackedFloat64Array", "PackedStringArray", "PackedVector2Array", "PackedVector3Array",
		"PackedColorArray", "PackedVector4Array"
	};
	if (target == "Array" && packed_arrays.contains(source)) return true;
	if (source == "Array" && packed_arrays.contains(target)) return true;
	return false;
}

} // namespace

bool Workspace::open(const std::filesystem::path &root, const std::filesystem::path &api_path, std::string *error) {
	auto started = std::chrono::steady_clock::now();
	std::unique_lock lock(mutex_);
	root_ = std::filesystem::weakly_canonical(root);
	if (!std::filesystem::exists(root_ / "project.godot")) {
		if (error) *error = "project.godot not found beneath " + root_.string();
		return false;
	}
	documents_.clear();
	disk_sources_.clear();
	classes_.clear();
	global_classes_.clear();
	global_name_counts_.clear();
	autoloads_.clear();
	uid_paths_.clear();
	unsafe_property_access_ = WarningLevel::Ignore;
	unsafe_method_access_ = WarningLevel::Ignore;
	native_api_ = {};
	stats_ = {};

	std::filesystem::path metadata;
	if (!api_path.empty()) {
		metadata = api_path;
		if (!std::filesystem::exists(metadata)) {
			if (error) *error = "native API not found: " + metadata.string();
			return false;
		}
	} else {
		for (const auto &candidate : {
				root_ / ".godot/addons/gdscript_parser/extension_api.json",
				root_ / "addons/gdscript_lsp/data/godot-4.6-extension-api.json"}) {
			if (std::filesystem::exists(candidate)) {
				metadata = candidate;
				break;
			}
		}
	}
	if (!metadata.empty()) {
		std::string api_error;
		if (!native_api_.load(metadata, &api_error)) {
			if (error) *error = "native API: " + api_error;
			return false;
		}
	}
	read_project_settings();
	scan_uid_files();

	std::error_code ec;
	for (std::filesystem::recursive_directory_iterator iterator(root_, std::filesystem::directory_options::skip_permission_denied, ec), end;
			iterator != end; iterator.increment(ec)) {
		if (ec) {
			ec.clear();
			continue;
		}
		if (iterator->is_directory()) {
			if (iterator->path().filename() == ".git" || std::filesystem::exists(iterator->path() / ".gdignore")) {
				iterator.disable_recursion_pending();
			}
			continue;
		}
		if (iterator->path().extension() != ".gd") continue;
		auto source = read_file(iterator->path());
		auto uri = uri_for_path(iterator->path());
		disk_sources_[uri] = source;
		auto document = std::make_shared<Document>(uri, resource_path(iterator->path()), std::move(source));
		stats_.syntax_error_count += document->syntax_errors().size();
		documents_[uri] = std::move(document);
	}
	rebuild_registry();
	stats_.document_count = documents_.size();
	stats_.class_count = classes_.size();
	stats_.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	return true;
}

void Workspace::scan_uid_files() {
	std::error_code ec;
	for (std::filesystem::recursive_directory_iterator iterator(root_, std::filesystem::directory_options::skip_permission_denied, ec), end;
			iterator != end; iterator.increment(ec)) {
		if (ec) {
			ec.clear();
			continue;
		}
		if (iterator->is_directory()) {
			if (iterator->path().filename() == ".git" || std::filesystem::exists(iterator->path() / ".gdignore")) {
				iterator.disable_recursion_pending();
			}
			continue;
		}
		if (!iterator->path().string().ends_with(".gd.uid")) continue;
		auto uid = trim(read_file(iterator->path()));
		auto script_path = iterator->path();
		script_path.replace_extension();
		if (!uid.empty()) uid_paths_[uid] = resource_path(script_path);
	}
}

void Workspace::read_project_settings() {
	autoloads_.clear();
	unsafe_property_access_ = WarningLevel::Ignore;
	unsafe_method_access_ = WarningLevel::Ignore;
	std::istringstream stream(read_file(root_ / "project.godot"));
	std::string line;
	std::string section;
	while (std::getline(stream, line)) {
		auto clean = trim(line);
		if (clean.size() >= 2 && clean.front() == '[' && clean.back() == ']') {
			section = trim(std::string_view(clean).substr(1, clean.size() - 2));
			continue;
		}
		auto separator = clean.find('=');
		if (separator == std::string::npos) continue;
		auto name = trim(clean.substr(0, separator));
		auto value = trim(clean.substr(separator + 1));
		if (section == "autoload") {
			if (!is_identifier(name) || value.size() < 2 || value.front() != '"' || value.back() != '"') continue;
			value = value.substr(1, value.size() - 2);
			if (value.starts_with('*')) value.erase(value.begin());
			if (!value.empty()) autoloads_[name] = value;
		} else if (section == "debug") {
			auto warning_level = [&]() {
				if (value == "1") return WarningLevel::Warning;
				if (value == "2") return WarningLevel::Error;
				return WarningLevel::Ignore;
			}();
			if (name == "gdscript/warnings/unsafe_property_access") unsafe_property_access_ = warning_level;
			else if (name == "gdscript/warnings/unsafe_method_access") unsafe_method_access_ = warning_level;
		}
	}
}

void Workspace::rebuild_registry() {
	classes_.clear();
	global_classes_.clear();
	global_name_counts_.clear();
	symbol_owners_.clear();
	static_symbol_types_.clear();
	for (auto &[uri, document] : documents_) {
		(void)uri;
		for (auto &record : document->classes()) {
			classes_[record.symbol.id] = &record;
			if (!record.global_name.empty()) ++global_name_counts_[record.global_name];
			for (const auto &member : record.members) symbol_owners_[member.id] = record.symbol.id;
		}
	}
	for (auto &[id, record] : classes_) {
		if (!record->global_name.empty() && global_name_counts_[record->global_name] == 1) {
			global_classes_[record->global_name] = id;
		}
	}
	for (auto &[id, record] : classes_) {
		(void)id;
		record->base_class_id.clear();
		record->inheritance_error.clear();
	}
	// Base expressions may depend on constants exported by another script, and
	// those constants may themselves be aliases. Repeat until inheritance-backed
	// lookups cannot make any further progress; recursive alias cycles are guarded
	// independently by symbol IDs.
	for (size_t pass = 0; pass <= classes_.size(); ++pass) {
		bool changed = false;
		for (auto &[id, record] : classes_) {
			(void)id;
			if (!record->base_class_id.empty()) continue;
			auto extension = trim(record->extends_text);
			if (extension.empty()) extension = "RefCounted";
			std::unordered_set<std::string> stack;
			auto resolved = resolve_static_reference(extension, record, stack);
			if (resolved.kind == TypeKind::ScriptClass) record->base_class_id = resolved.symbol_id;
			else if (resolved.kind == TypeKind::NativeClass) record->base_class_id = "native:" + resolved.name;
			changed = changed || !record->base_class_id.empty();
		}
		if (!changed) break;
	}
	for (auto &[id, record] : classes_) {
		(void)id;
		if (record->base_class_id.empty()) {
			auto extension = trim(record->extends_text);
			if (extension.empty()) extension = "RefCounted";
			record->inheritance_error = "Could not resolve super class \"" + extension + "\".";
		}
	}
	std::unordered_set<std::string> cyclic_classes;
	for (const auto &[id, record] : classes_) {
		(void)record;
		std::vector<std::string> path;
		std::unordered_map<std::string, size_t> positions;
		auto current_id = id;
		while (!current_id.empty() && !current_id.starts_with("native:") && classes_.contains(current_id)) {
			if (auto repeated = positions.find(current_id); repeated != positions.end()) {
				for (size_t index = repeated->second; index < path.size(); ++index) cyclic_classes.insert(path[index]);
				break;
			}
			positions[current_id] = path.size();
			path.push_back(current_id);
			current_id = classes_[current_id]->base_class_id;
		}
	}
	for (const auto &id : cyclic_classes) {
		auto *record = classes_[id];
		record->inheritance_error = "Could not resolve class \"" + record->symbol.name + "\": Cyclic reference.";
		record->base_class_id.clear();
	}
	for (const auto &[id, record] : classes_) {
		(void)id;
		for (const auto &member : record->members) {
			std::unordered_set<std::string> stack;
			auto type = resolve_static_symbol(member, stack);
			if (type.known()) static_symbol_types_[member.id] = std::move(type);
		}
	}
}

bool Workspace::update_document(const std::string &uri, std::string text, int64_t version, std::string *error) {
	std::unique_lock lock(mutex_);
	auto path = path_for_uri(uri);
	if (path.empty()) {
		if (error) *error = "invalid file URI";
		return false;
	}
	auto relative = path.lexically_normal().lexically_relative(root_.lexically_normal());
	if (relative.empty() || relative == ".." || (!relative.empty() && *relative.begin() == "..")) {
		if (error) *error = "document is outside workspace";
		return false;
	}
	if (!disk_sources_.contains(uri)) disk_sources_[uri] = read_file(path);
	documents_[uri] = std::make_shared<Document>(uri, resource_path(path), std::move(text), version);
	rebuild_registry();
	return true;
}

bool Workspace::close_document(const std::string &uri, std::string *error) {
	std::unique_lock lock(mutex_);
	auto found = disk_sources_.find(uri);
	if (found == disk_sources_.end()) {
		if (error) *error = "document not indexed";
		return false;
	}
	documents_[uri] = std::make_shared<Document>(uri, resource_path(path_for_uri(uri)), found->second);
	rebuild_registry();
	return true;
}

bool Workspace::refresh_file(const std::string &uri, std::string *error) {
	std::unique_lock lock(mutex_);
	auto path = path_for_uri(uri);
	if (path.lexically_normal() == (root_ / "project.godot").lexically_normal()) {
		read_project_settings();
		rebuild_registry();
		return true;
	}
	if (!std::filesystem::exists(path)) {
		documents_.erase(uri);
		disk_sources_.erase(uri);
		rebuild_registry();
		return true;
	}
	auto source = read_file(path);
	if (source.empty() && std::filesystem::file_size(path) != 0) {
		if (error) *error = "cannot read file";
		return false;
	}
	disk_sources_[uri] = source;
	documents_[uri] = std::make_shared<Document>(uri, resource_path(path), std::move(source));
	rebuild_registry();
	return true;
}

std::string Workspace::resource_path(const std::filesystem::path &path) const {
	auto relative = std::filesystem::weakly_canonical(path).lexically_relative(root_).generic_string();
	return "res://" + relative;
}

std::string Workspace::uri_for_path(const std::filesystem::path &path) const {
	return file_uri_for_path(path);
}

std::filesystem::path Workspace::path_for_uri(const std::string &uri) const {
	if (auto path = path_for_file_uri(uri)) return *path;
	if (uri.starts_with("res://")) return (root_ / uri.substr(6)).lexically_normal();
	return {};
}

std::string Workspace::resolve_path_reference(std::string reference, std::string_view owner_resource) const {
	reference = unquote(reference);
	if (reference.starts_with("uid://")) {
		auto found = uid_paths_.find(reference);
		return found == uid_paths_.end() ? reference : found->second;
	}
	if (reference.starts_with("res://")) return reference;
	auto owner = std::string(owner_resource);
	auto suffix = owner.find(".gd.");
	if (suffix != std::string::npos) owner.resize(suffix + 3);
	auto base = std::filesystem::path(owner.substr(6)).parent_path();
	return "res://" + (base / reference).lexically_normal().generic_string();
}

const Document *Workspace::find_document(const std::string &uri) const {
	auto found = documents_.find(uri);
	return found == documents_.end() ? nullptr : found->second.get();
}

const ClassRecord *Workspace::find_class(std::string_view id) const {
	auto found = classes_.find(std::string(id));
	return found == classes_.end() ? nullptr : found->second;
}

ResolvedType Workspace::resolve_static_symbol(const Symbol &symbol, std::unordered_set<std::string> &stack) const {
	if (auto cached = static_symbol_types_.find(symbol.id); cached != static_symbol_types_.end()) return cached->second;
	if (!stack.insert(symbol.id).second) return ResolvedType::unknown("static reference cycle");
	ResolvedType result;
	if (symbol.kind == SymbolKind::Class && classes_.contains(symbol.declared_type)) {
		auto *record = find_class(symbol.declared_type);
		result = {TypeKind::ScriptClass, record ? record->symbol.name : symbol.name, symbol.declared_type, false};
	} else if (symbol.kind == SymbolKind::Enum) {
		result = {TypeKind::Enum, symbol.name, symbol.id, false};
	} else if (symbol.kind == SymbolKind::Constant && !symbol.initializer.empty()) {
		auto initializer = trim(symbol.initializer);
		if (initializer.starts_with("&\"") || initializer.starts_with("&'")) {
			result = {TypeKind::Builtin, "StringName"};
		} else if (initializer.starts_with('"') || initializer.starts_with('\'')) {
			result = {TypeKind::Builtin, "String"};
		} else {
			auto owner = symbol_owners_.find(symbol.id);
			auto *context = owner == symbol_owners_.end() ? nullptr : find_class(owner->second);
			result = resolve_static_reference(initializer, context, stack);
		}
	}
	stack.erase(symbol.id);
	return result;
}

ResolvedType Workspace::resolve_static_reference(std::string expression, const ClassRecord *context,
		std::unordered_set<std::string> &stack) const {
	expression = trim(expression);
	if (expression.empty()) return ResolvedType::unknown();

	ResolvedType current;
	std::string suffix;
	auto resolve_path = [&](std::string reference) {
		auto resolved = resolve_path_reference(std::move(reference), context ? context->symbol.id : "res://");
		auto *record = find_class(resolved);
		return record ? ResolvedType{TypeKind::ScriptClass, record->symbol.name, resolved, false} :
			ResolvedType::unknown(resolved);
	};

	bool loaded_path = false;
	for (auto loader : {std::string_view("preload"), std::string_view("load")}) {
		if (!expression.starts_with(loader)) continue;
		auto open = expression.find('(', loader.size());
		auto close = expression.find(')', open == std::string::npos ? 0 : open + 1);
		if (open == std::string::npos || close == std::string::npos) return ResolvedType::unknown(expression);
		auto argument = trim(std::string_view(expression).substr(open + 1, close - open - 1));
		if (argument.size() < 2 || (argument.front() != '"' && argument.front() != '\'') || argument.back() != argument.front()) {
			return ResolvedType::unknown(expression);
		}
		current = resolve_path(argument);
		suffix = trim(std::string_view(expression).substr(close + 1));
		loaded_path = true;
		break;
	}
	if (!loaded_path && (expression.front() == '"' || expression.front() == '\'' ||
			expression.starts_with("uid://") || expression.starts_with("res://"))) {
		current = resolve_path(expression);
		loaded_path = true;
	}

	std::vector<std::string> parts;
	auto split_parts = [&](std::string_view value) {
		if (!value.empty() && value.front() == '.') value.remove_prefix(1);
		while (!value.empty()) {
			auto dot = value.find('.');
			auto part = trim(value.substr(0, dot));
			if (!is_identifier(part)) return false;
			parts.push_back(std::move(part));
			if (dot == std::string_view::npos) break;
			value.remove_prefix(dot + 1);
		}
		return true;
	};
	if (loaded_path) {
		if (!suffix.empty() && !split_parts(suffix)) return ResolvedType::unknown(expression);
	} else {
		if (!split_parts(expression) || parts.empty()) return ResolvedType::unknown(expression);
		auto root = parts.front();
		parts.erase(parts.begin());
		for (auto *scope = context; scope && !current.known();) {
			if (auto *member = find_member(*scope, root)) current = resolve_static_symbol(*member, stack);
			if (!current.known()) {
				auto inner_id = scope->symbol.id + "." + root;
				if (auto *inner = find_class(inner_id)) current = {TypeKind::ScriptClass, inner->symbol.name, inner_id, false};
			}
			auto separator = scope->symbol.id.rfind('.');
			auto script_end = scope->symbol.id.find(".gd");
			if (separator == std::string::npos || script_end == std::string::npos || separator <= script_end + 2) break;
			scope = find_class(scope->symbol.id.substr(0, separator));
		}
		if (!current.known()) if (auto found = global_classes_.find(root); found != global_classes_.end()) {
			current = {TypeKind::ScriptClass, root, found->second, false};
		}
		if (!current.known() && native_api_.has_class(root)) {
			if (root == "Callable") current = {TypeKind::Callable, root, {}, false};
			else if (root == "Signal") current = {TypeKind::Signal, root, {}, false};
			else if (native_api_.is_builtin_class(root)) current = {TypeKind::Builtin, root, {}, false};
			else current = {TypeKind::NativeClass, root, "native:" + root, false};
		}
	}

	for (const auto &part : parts) {
		if (current.kind == TypeKind::ScriptClass) {
			auto *record = find_class(current.symbol_id);
			if (!record) return ResolvedType::unknown(expression);
			auto *member = find_member(*record, part);
			if (!member) return ResolvedType::unknown(expression);
			current = resolve_static_symbol(*member, stack);
		} else if (current.kind == TypeKind::Enum) {
			bool found = false;
			for (const auto &[id, record] : classes_) {
				(void)id;
				for (const auto &member : record->members) {
					if (member.id != current.symbol_id) continue;
					found = std::any_of(member.children.begin(), member.children.end(),
						[&](const Symbol &value) { return value.name == part; });
					break;
				}
				if (found) break;
			}
			if (!found) return ResolvedType::unknown(expression);
			current = {TypeKind::Builtin, "int"};
		} else {
			return ResolvedType::unknown(expression);
		}
		if (!current.known()) return ResolvedType::unknown(expression);
	}
	return current;
}

std::vector<const Symbol *> Workspace::all_members(const ClassRecord &record) const {
	std::vector<const Symbol *> result;
	std::unordered_set<std::string> names;
	std::unordered_set<std::string> classes_seen;
	auto *current = &record;
	while (current && classes_seen.insert(current->symbol.id).second) {
		for (const auto &member : current->members) {
			if (names.insert(member.name).second) result.push_back(&member);
		}
		if (current->base_class_id.starts_with("native:")) break;
		current = find_class(current->base_class_id);
	}
	return result;
}

const Symbol *Workspace::find_member(const ClassRecord &record, std::string_view name) const {
	for (auto *member : all_members(record)) if (member->name == name) return member;
	return nullptr;
}

std::string Workspace::native_base(const ClassRecord &record) const {
	std::unordered_set<std::string> seen;
	auto *current = &record;
	while (current && seen.insert(current->symbol.id).second) {
		if (current->base_class_id.starts_with("native:")) return current->base_class_id.substr(7);
		current = find_class(current->base_class_id);
	}
	return {};
}

ResolvedType Workspace::type_from_name(std::string name, const ClassRecord *context) const {
	name = normalize_api_type(trim(name));
	if (name.empty()) return ResolvedType::unknown();
	if (name == "void") return {TypeKind::Void, "void"};
	if (name == "Variant" || name == "Nil") return {TypeKind::Variant, "Variant"};
	if (name.starts_with("Array[") && name.ends_with(']')) {
		ResolvedType result{TypeKind::Builtin, "Array"};
		result.arguments.push_back(type_from_name(name.substr(6, name.size() - 7), context));
		return result;
	}
	if (name.starts_with("Dictionary[") && name.ends_with(']')) {
		auto arguments = std::string_view(name).substr(11, name.size() - 12);
		size_t separator = std::string_view::npos;
		int depth = 0;
		for (size_t index = 0; index < arguments.size(); ++index) {
			if (arguments[index] == '[') ++depth;
			else if (arguments[index] == ']') --depth;
			else if (arguments[index] == ',' && depth == 0) {
				separator = index;
				break;
			}
		}
		ResolvedType result{TypeKind::Builtin, "Dictionary"};
		if (separator != std::string_view::npos) {
			result.arguments.push_back(type_from_name(std::string(arguments.substr(0, separator)), context));
			result.arguments.push_back(type_from_name(std::string(arguments.substr(separator + 1)), context));
		}
		return result;
	}
	if (name.starts_with("res://")) {
		auto *record = find_class(name);
		return {TypeKind::ScriptClass, record ? record->symbol.name : name, name, true};
	}
	if (global_classes_.contains(name)) return {TypeKind::ScriptClass, name, global_classes_.at(name), true};
	if (context && classes_.contains(context->symbol.id + "." + name)) {
		return {TypeKind::ScriptClass, name, context->symbol.id + "." + name, true};
	}
	if (context) {
		std::unordered_set<std::string> stack;
		auto resolved = resolve_static_reference(name, context, stack);
		if (resolved.known()) {
			if (resolved.kind == TypeKind::ScriptClass || resolved.kind == TypeKind::NativeClass) resolved.instance = true;
			return resolved;
		}
	}
	if (name == "Callable") return {TypeKind::Callable, name};
	if (name == "Signal") return {TypeKind::Signal, name};
	if (native_api_.is_global_enum(name)) return {TypeKind::Enum, name, "global:" + name, false};
	if (auto dot = name.rfind('.'); dot != std::string::npos &&
			native_api_.has_enum(name.substr(0, dot), name.substr(dot + 1))) {
		return {TypeKind::Enum, name.substr(dot + 1), "nativeenum:" + name, false};
	}
	static const std::unordered_set<std::string> builtins = {
		"bool", "int", "float", "String", "StringName", "NodePath", "Array", "Dictionary",
		"Vector2", "Vector2i", "Vector3", "Vector3i", "Color"
	};
	if (builtins.contains(name)) return {TypeKind::Builtin, name};
	if (native_api_.is_builtin_class(name)) return {TypeKind::Builtin, name};
	if (native_api_.has_class(name)) return {TypeKind::NativeClass, name, "native:" + name, true};
	return ResolvedType::unknown(name);
}

const Symbol *Workspace::resolve_identifier(const Document &document, const ClassRecord *context,
		std::string_view name, Position position) const {
	if (auto *local = document.find_local(name, position)) return local;
	if (context) if (auto *member = find_member(*context, name)) return member;
	return nullptr;
}

ResolvedType Workspace::type_of_symbol(const Symbol &symbol, const Document &document, Position position,
		std::vector<std::string> &stack) const {
	if (auto cached = static_symbol_types_.find(symbol.id); cached != static_symbol_types_.end()) return cached->second;
	if (std::find(stack.begin(), stack.end(), symbol.id) != stack.end()) return ResolvedType::unknown("cycle");
	stack.push_back(symbol.id);
	auto *declaration_document = find_document(symbol.uri);
	if (!declaration_document) declaration_document = &document;
	auto *context = declaration_document->class_at(symbol.range.start);
	ResolvedType result;
	if (!symbol.declared_type.empty()) result = type_from_name(symbol.declared_type, context);
	else if (symbol.kind == SymbolKind::Event) result = {TypeKind::Signal, "Signal"};
	else if (symbol.kind == SymbolKind::Enum) result = {TypeKind::Enum, symbol.name, symbol.id};
	else if (!symbol.initializer.empty()) result = infer_expression(symbol.initializer, *declaration_document, context,
		declaration_document == &document ? position : symbol.range.start, stack);
	else if (symbol.kind == SymbolKind::Method || symbol.kind == SymbolKind::Constructor) result = {TypeKind::Variant, "Variant"};
	else result = {TypeKind::Variant, "Variant"};
	stack.pop_back();
	return result;
}

ResolvedType Workspace::infer_expression(std::string expression, const Document &document, const ClassRecord *context,
		Position position, std::vector<std::string> &stack) const {
	expression = trim(expression);
	while (expression.size() >= 2 && expression.front() == '(' && expression.back() == ')') {
		expression = trim(expression.substr(1, expression.size() - 2));
	}
	if (expression.empty()) return ResolvedType::unknown();
	if (expression == "true" || expression == "false") return {TypeKind::Builtin, "bool"};
	if (expression == "null") return {TypeKind::Variant, "Variant"};
	if (expression.front() == '"' || expression.front() == '\'' || expression.starts_with("&\"") || expression.starts_with("&'")) {
		return {TypeKind::Builtin, expression.front() == '&' ? "StringName" : "String"};
	}
	if (expression.front() == '[') return {TypeKind::Builtin, "Array"};
	if (expression.front() == '{') return {TypeKind::Builtin, "Dictionary"};
	if (is_integer_literal(expression)) return {TypeKind::Builtin, "int"};
	if (is_float_literal(expression)) return {TypeKind::Builtin, "float"};
	for (auto loader : {std::string_view("preload"), std::string_view("load")}) {
		if (!expression.starts_with(loader)) continue;
		auto call = trim(std::string_view(expression).substr(loader.size()));
		if (call.size() < 4 || call.front() != '(' || call.back() != ')') continue;
		auto argument = trim(std::string_view(call).substr(1, call.size() - 2));
		if (argument.size() < 2 || (argument.front() != '"' && argument.front() != '\'') || argument.back() != argument.front()) continue;
		auto id = resolve_path_reference(argument, document.resource_path());
		auto result = type_from_name(id, context);
		result.instance = false;
		return result;
	}
	if (auto call = member_call(expression); call && call->second == "new") {
		auto result = type_from_name(call->first, context);
		if (!result.known()) {
			if (auto *symbol = resolve_identifier(document, context, call->first, position)) {
				result = type_of_symbol(*symbol, document, position, stack);
			}
		}
		result.instance = true;
		return result;
	}
	if (auto call = member_call(expression)) {
		ResolvedType receiver;
		if (auto *symbol = resolve_identifier(document, context, call->first, position)) {
			receiver = type_of_symbol(*symbol, document, position, stack);
		} else {
			receiver = type_from_name(call->first, context);
		}
		if (receiver.kind == TypeKind::ScriptClass) {
			if (auto *record = find_class(receiver.symbol_id)) {
				if (auto *member = find_member(*record, call->second)) return type_of_symbol(*member, document, position, stack);
			}
		} else if (receiver.kind == TypeKind::NativeClass || receiver.kind == TypeKind::Builtin) {
			if (auto *member = native_api_.find_member(receiver.name, call->second)) return type_from_name(member->type, context);
		}
	}
	if (auto call = function_call(expression)) {
		if (auto *symbol = resolve_identifier(document, context, *call, position)) {
			return type_of_symbol(*symbol, document, position, stack);
		}
	}
	if (is_identifier(expression)) {
		if (auto found = autoloads_.find(expression); found != autoloads_.end()) {
			auto result = type_from_name(resolve_path_reference(found->second, document.resource_path()), context);
			result.instance = true;
			return result;
		}
		if (auto *symbol = resolve_identifier(document, context, expression, position)) {
			return type_of_symbol(*symbol, document, position, stack);
		}
		return type_from_name(expression, context);
	}
	{
		std::unordered_set<std::string> static_stack;
		auto result = resolve_static_reference(expression, context, static_stack);
		if (result.known()) return result;
	}
	auto cast = expression.rfind(" as ");
	if (cast != std::string::npos) return type_from_name(expression.substr(cast + 4), context);
	return ResolvedType::unknown(expression);
}

ResolvedType Workspace::resolve_type(const std::string &uri, Position position, std::string expression) const {
	std::shared_lock lock(mutex_);
	auto *document = find_document(uri);
	if (!document) return ResolvedType::unknown("document not indexed");
	if (expression.empty()) expression = identifier_at(document->source(), position);
	std::vector<std::string> stack;
	return infer_expression(std::move(expression), *document, document->class_at(position), position, stack);
}

bool Workspace::is_assignable(const ResolvedType &expected, const ResolvedType &actual) const {
	if (expected.kind == TypeKind::Variant || actual.kind == TypeKind::Variant || !expected.known() || !actual.known()) return true;
	if (expected.kind == actual.kind && (expected.name == actual.name ||
			(!expected.symbol_id.empty() && expected.symbol_id == actual.symbol_id))) {
		if ((expected.kind != TypeKind::ScriptClass && expected.kind != TypeKind::NativeClass) ||
				expected.instance == actual.instance) return true;
	}
	if (expected.kind == TypeKind::Builtin && actual.kind == TypeKind::Builtin &&
			can_convert_strict(actual.name, expected.name)) return true;
	if (expected.kind == TypeKind::Builtin && expected.name == "RID" && actual.kind == TypeKind::NativeClass) return true;
	if (expected.kind == TypeKind::Enum && actual.kind == TypeKind::Builtin && actual.name == "int") return true;
	if (expected.kind == TypeKind::Builtin && expected.name == "int" && actual.kind == TypeKind::Enum) return true;
	if (actual.kind == TypeKind::ScriptClass) {
		if (!actual.instance) {
			if (expected.kind != TypeKind::NativeClass) return false;
			std::string meta_type = native_api_.has_class("GDScript") ? "GDScript" : "Script";
			return is_assignable(expected, {TypeKind::NativeClass, meta_type, "native:" + meta_type, true});
		}
		auto *current = find_class(actual.symbol_id);
		std::unordered_set<std::string> seen;
		while (current && seen.insert(current->symbol.id).second) {
			if (expected.kind == TypeKind::ScriptClass && current->symbol.id == expected.symbol_id) return true;
			if (current->base_class_id.starts_with("native:")) {
				ResolvedType native{TypeKind::NativeClass, current->base_class_id.substr(7), current->base_class_id};
				return is_assignable(expected, native);
			}
			current = find_class(current->base_class_id);
		}
	}
	if (expected.kind == TypeKind::NativeClass && actual.kind == TypeKind::NativeClass) {
		std::unordered_set<std::string> seen;
		auto current = actual.name;
		while (!current.empty() && seen.insert(current).second) {
			if (current == expected.name) return true;
			auto *record = native_api_.find_class(current);
			current = record ? record->parent : std::string{};
		}
	}
	return false;
}

std::vector<CompletionItem> Workspace::completion(const std::string &uri, Position position) const {
	std::shared_lock lock(mutex_);
	std::vector<CompletionItem> result;
	auto *document = find_document(uri);
	if (!document) return result;
	auto offset = position_to_byte(document->source(), position);
	auto line_start = document->source().rfind('\n', offset == 0 ? 0 : offset - 1);
	line_start = line_start == std::string::npos ? 0 : line_start + 1;
	auto prefix = document->source().substr(line_start, offset - line_start);
	std::set<std::string> names;
	auto add_symbol = [&](const Symbol &symbol) {
		if (!names.insert(symbol.name).second) return;
		result.push_back({symbol.name, symbol.detail, symbol.documentation, symbol.kind, symbol.name});
	};
	if (auto receiver_text = completion_receiver(prefix)) {
		std::vector<std::string> stack;
		auto *context = document->class_at(position);
		auto receiver = infer_expression(*receiver_text, *document, context, position, stack);
		if (receiver.kind == TypeKind::ScriptClass) {
			if (auto *record = find_class(receiver.symbol_id)) {
				for (auto *member : all_members(*record)) add_symbol(*member);
				auto base = receiver.instance ? native_base(*record) :
					(native_api_.has_class("GDScript") ? std::string("GDScript") : std::string("Script"));
				if (!base.empty()) {
					for (auto *member : native_api_.members(base)) {
						if (names.insert(member->name).second) {
							result.push_back({member->name, member->detail, member->documentation, member->kind, member->name});
						}
					}
				}
			}
		} else if (receiver.kind == TypeKind::NativeClass || receiver.kind == TypeKind::Builtin) {
			for (auto *member : native_api_.members(receiver.name)) {
				if (names.insert(member->name).second) {
					result.push_back({member->name, member->detail, member->documentation, member->kind, member->name});
				}
			}
		}
	} else {
		for (auto *local : document->locals_at(position)) add_symbol(*local);
		if (auto *record = document->class_at(position)) {
			for (auto *member : all_members(*record)) add_symbol(*member);
			auto base = native_base(*record);
			if (!base.empty()) {
				for (auto *member : native_api_.members(base)) {
					if (names.insert(member->name).second) {
						result.push_back({member->name, member->detail, member->documentation, member->kind, member->name});
					}
				}
			}
		}
		for (const auto &[name, id] : global_classes_) {
			(void)id;
			if (names.insert(name).second) result.push_back({name, "class " + name, {}, SymbolKind::Class, name});
		}
		for (const auto &[name, path] : autoloads_) {
			if (names.insert(name).second) result.push_back({name, "autoload " + path, {}, SymbolKind::Variable, name});
		}
		if (names.insert("is_instance_of").second) {
			result.push_back({"is_instance_of", "func is_instance_of(value: Variant, type: Variant) -> bool", {},
				SymbolKind::Method, "is_instance_of"});
		}
	}
	std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.label < b.label; });
	return result;
}

std::optional<HoverResult> Workspace::hover(const std::string &uri, Position position) const {
	std::shared_lock lock(mutex_);
	auto *document = find_document(uri);
	if (!document) return std::nullopt;
	auto name = identifier_at(document->source(), position);
	if (name.empty()) return std::nullopt;
	auto *context = document->class_at(position);
	if (auto *symbol = resolve_identifier(*document, context, name, position)) {
		std::vector<std::string> stack;
		auto type = type_of_symbol(*symbol, *document, position, stack);
		auto declaration = symbol->detail.empty() ? symbol->name + ": " + type.display() : symbol->detail;
		return HoverResult{"**" + declaration + "**\n\nDeclared in " + symbol->id, symbol->selection_range};
	}
	if (global_classes_.contains(name)) {
		auto *record = find_class(global_classes_.at(name));
		return HoverResult{"**class " + name + " extends " + (record ? record->extends_text : "Unknown") + "**",
			record ? record->symbol.selection_range : Range{}};
	}
	if (auto *native = native_api_.find_class(name)) {
		return HoverResult{"**class " + native->name + (native->parent.empty() ? "" : " extends " + native->parent) + "**", {}};
	}
	return std::nullopt;
}

std::vector<Location> Workspace::definition(const std::string &uri, Position position) const {
	std::shared_lock lock(mutex_);
	std::vector<Location> result;
	auto *document = find_document(uri);
	if (!document) return result;
	auto name = identifier_at(document->source(), position);
	if (name.empty()) return result;
	if (auto *symbol = resolve_identifier(*document, document->class_at(position), name, position)) {
		result.push_back({symbol->uri, symbol->selection_range});
	} else if (global_classes_.contains(name)) {
		auto *record = find_class(global_classes_.at(name));
		if (record) result.push_back({record->symbol.uri, record->symbol.selection_range});
	} else if (autoloads_.contains(name)) {
		auto id = resolve_path_reference(autoloads_.at(name), document->resource_path());
		if (auto *record = find_class(id)) result.push_back({record->symbol.uri, record->symbol.selection_range});
	}
	return result;
}

std::vector<Symbol> Workspace::document_symbols(const std::string &uri) const {
	std::shared_lock lock(mutex_);
	std::vector<Symbol> result;
	auto *document = find_document(uri);
	if (!document) return result;
	for (const auto &record : document->classes()) {
		Symbol symbol = record.symbol;
		symbol.detail = "class " + symbol.name + " extends " + record.extends_text;
		symbol.children = record.members;
		result.push_back(std::move(symbol));
	}
	return result;
}

std::vector<std::string> Workspace::document_uris() const {
	std::shared_lock lock(mutex_);
	std::vector<std::string> result;
	result.reserve(documents_.size());
	for (const auto &[uri, document] : documents_) {
		(void)document;
		result.push_back(uri);
	}
	std::sort(result.begin(), result.end());
	return result;
}

int64_t Workspace::document_version(const std::string &uri) const {
	std::shared_lock lock(mutex_);
	auto *document = find_document(uri);
	return document ? document->version() : -1;
}

std::vector<Diagnostic> Workspace::diagnostics(const std::string &uri) const {
	std::shared_lock lock(mutex_);
	std::vector<Diagnostic> result;
	auto *document = find_document(uri);
	if (!document) return result;
	auto add = [&](std::string code, std::string message, Range range,
			DiagnosticSeverity severity = DiagnosticSeverity::Error) {
		result.push_back({std::move(code), std::move(message), range, severity});
	};
	for (auto range : document->syntax_errors()) add("syntax-error", "Syntax error.", range);
	for (const auto &record : document->classes()) {
		if (!record.global_name.empty() && global_name_counts_.contains(record.global_name) &&
				global_name_counts_.at(record.global_name) > 1) {
			add("duplicate-global-class", "Global class \"" + record.global_name + "\" is declared more than once.",
				record.symbol.selection_range);
		}
		if (!record.inheritance_error.empty()) add(record.inheritance_error.find("Cyclic") != std::string::npos ?
			"inheritance-cycle" : "unresolved-base", record.inheritance_error, record.symbol.range);
		std::unordered_map<std::string, const Symbol *> declarations;
		for (const auto &member : record.members) {
			auto [existing, inserted] = declarations.emplace(member.name, &member);
			if (!inserted) {
				Diagnostic diagnostic{"duplicate-symbol", "Member \"" + member.name + "\" is declared more than once.",
					member.selection_range};
				diagnostic.related_information.push_back({{existing->second->uri, existing->second->selection_range},
					"First declaration is here."});
				result.push_back(std::move(diagnostic));
			}
			auto inspect_symbol = [&](const Symbol &symbol) {
				if (!symbol.declared_type.empty() && !type_from_name(symbol.declared_type, &record).known()) {
					add("unknown-type", "Could not find type \"" + symbol.declared_type + "\" in the current scope.",
						symbol.selection_range);
					return;
				}
			};
			inspect_symbol(member);
			for (const auto &local : member.children) inspect_symbol(local);
		}
	}
	if (document->syntax_errors().empty()) {
		auto semantic = SemanticAnalyzer::run(*this, *document);
		result.insert(result.end(), std::make_move_iterator(semantic.begin()), std::make_move_iterator(semantic.end()));
	}
	std::sort(result.begin(), result.end(), [](const Diagnostic &a, const Diagnostic &b) {
		if (a.range.start != b.range.start) return a.range.start < b.range.start;
		return a.code < b.code;
	});
	return result;
}

} // namespace gdscript_lsp
