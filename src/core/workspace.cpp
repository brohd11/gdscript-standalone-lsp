#include "core/workspace.hpp"
#include "core/caret_context.hpp"
#include "core/gdscript_api.hpp"
#include "core/semantic_analyzer.hpp"
#include "core/text.hpp"
#include "core/uri.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
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

struct TerminalCall {
	std::string callee;
	std::string arguments;
};

std::optional<TerminalCall> terminal_call(std::string value) {
	value = trim(value);
	if (value.empty() || value.back() != ')') return std::nullopt;
	size_t call_open = std::string::npos;
	int depth = 0;
	char quote = 0;
	bool escaped = false;
	for (size_t index = value.size(); index-- > 0;) {
		auto character = value[index];
		if (quote) {
			if (escaped) escaped = false;
			else if (character == '\\') escaped = true;
			else if (character == quote) quote = 0;
			continue;
		}
		if (character == '\'' || character == '"') {
			quote = character;
			continue;
		}
		if (character == ')') {
			++depth;
		} else if (character == '(') {
			if (depth == 0) return std::nullopt;
			if (--depth == 0) {
				call_open = index;
				break;
			}
		}
	}
	if (quote || depth != 0 || call_open == std::string::npos) return std::nullopt;
	auto callee = trim(std::string_view(value).substr(0, call_open));
	if (callee.empty()) return std::nullopt;
	return TerminalCall{std::move(callee),
		trim(std::string_view(value).substr(call_open + 1, value.size() - call_open - 2))};
}

std::optional<std::pair<std::string, std::string>> trailing_member(std::string_view expression) {
	int parentheses = 0;
	int brackets = 0;
	int braces = 0;
	char quote = 0;
	bool escaped = false;
	size_t separator = std::string_view::npos;
	for (size_t index = 0; index < expression.size(); ++index) {
		auto character = expression[index];
		if (quote) {
			if (escaped) escaped = false;
			else if (character == '\\') escaped = true;
			else if (character == quote) quote = 0;
			continue;
		}
		if (character == '\'' || character == '"') {
			quote = character;
			continue;
		}
		if (character == '(') ++parentheses;
		else if (character == ')') --parentheses;
		else if (character == '[') ++brackets;
		else if (character == ']') --brackets;
		else if (character == '{') ++braces;
		else if (character == '}') --braces;
		else if (character == '.' && parentheses == 0 && brackets == 0 && braces == 0) separator = index;
		if (parentheses < 0 || brackets < 0 || braces < 0) return std::nullopt;
	}
	if (quote || parentheses || brackets || braces || separator == std::string_view::npos) return std::nullopt;
	auto receiver = trim(expression.substr(0, separator));
	auto member = trim(expression.substr(separator + 1));
	if (receiver.empty() || !is_identifier(member)) return std::nullopt;
	return std::pair{std::move(receiver), std::move(member)};
}

std::optional<std::pair<std::string, std::string>> terminal_subscript(std::string_view expression) {
	if (expression.empty() || expression.back() != ']') return std::nullopt;
	int depth = 0;
	char quote = 0;
	bool escaped = false;
	size_t open = std::string_view::npos;
	for (size_t index = 0; index < expression.size(); ++index) {
		auto character = expression[index];
		if (quote) {
			if (escaped) escaped = false;
			else if (character == '\\') escaped = true;
			else if (character == quote) quote = 0;
			continue;
		}
		if (character == '\'' || character == '"') {
			quote = character;
			continue;
		}
		if (character == '[') {
			if (depth++ == 0) open = index;
		} else if (character == ']') {
			if (depth == 0 || --depth < 0) return std::nullopt;
			if (depth == 0 && index + 1 != expression.size()) return std::nullopt;
		}
	}
	if (quote || depth || open == std::string_view::npos) return std::nullopt;
	auto receiver = trim(expression.substr(0, open));
	auto key = trim(expression.substr(open + 1, expression.size() - open - 2));
	return receiver.empty() ? std::nullopt :
		std::optional<std::pair<std::string, std::string>>(std::pair{std::move(receiver), std::move(key)});
}

std::optional<std::string> quoted_value(std::string value) {
	value = trim(value);
	if (value.starts_with('&')) value.erase(value.begin());
	if (value.size() < 2 || (value.front() != '\'' && value.front() != '"') || value.back() != value.front()) {
		return std::nullopt;
	}
	return value.substr(1, value.size() - 2);
}

ResolvedType iterable_value_type(const ResolvedType &type) {
	if (type.kind != TypeKind::Builtin) return {TypeKind::Variant, "Variant"};
	if (type.name == "Dictionary" && !type.arguments.empty()) return type.arguments.front();
	if (type.name == "Array" && !type.arguments.empty()) return type.arguments.front();
	if (type.name == "String") return {TypeKind::Builtin, "String"};
	static const std::unordered_map<std::string, std::string> packed_elements = {
		{"PackedByteArray", "int"}, {"PackedInt32Array", "int"}, {"PackedInt64Array", "int"},
		{"PackedFloat32Array", "float"}, {"PackedFloat64Array", "float"},
		{"PackedStringArray", "String"}, {"PackedVector2Array", "Vector2"},
		{"PackedVector3Array", "Vector3"}, {"PackedVector4Array", "Vector4"},
		{"PackedColorArray", "Color"},
	};
	if (auto found = packed_elements.find(type.name); found != packed_elements.end()) {
		return {TypeKind::Builtin, found->second};
	}
	return {TypeKind::Variant, "Variant"};
}

struct ExpectedValue {
	ResolvedType type;
	std::string access;
	AccessProvenance provenance;
};

bool callable_kind(SymbolKind kind) {
	return kind == SymbolKind::Method || kind == SymbolKind::Function || kind == SymbolKind::Constructor;
}

CompletionItem completion_item(std::string name, std::string detail, std::string documentation,
		SymbolKind kind, bool has_arguments = false, std::string symbol_id = {}) {
	CompletionItem result;
	result.filter_text = name;
	result.documentation = std::move(documentation);
	result.kind = kind;
	result.symbol_id = std::move(symbol_id);
	result.origin_id = result.symbol_id;
	if (callable_kind(kind)) {
		result.label = name + (has_arguments ? "(\xe2\x80\xa6)" : "()");
		result.insert_text = name + (has_arguments ? "(" : "()");
		result.detail = std::move(detail);
	} else {
		result.label = name;
		result.detail = std::move(detail);
		result.insert_text = std::move(name);
	}
	return result;
}

CompletionItem completion_item(const Symbol &symbol) {
	auto has_arguments = std::any_of(symbol.children.begin(), symbol.children.end(),
		[](const Symbol &child) { return child.is_parameter; });
	std::string detail;
	switch (symbol.kind) {
		case SymbolKind::Method:
		case SymbolKind::Function:
		case SymbolKind::Constructor: detail = symbol.is_static ? "static func" : "func"; break;
		case SymbolKind::Constant: detail = "const"; break;
		case SymbolKind::Variable:
		case SymbolKind::Field:
		case SymbolKind::Property: detail = symbol.is_static ? "static var" : "var"; break;
		case SymbolKind::Event: detail = "signal"; break;
		case SymbolKind::Enum: detail = "enum"; break;
		case SymbolKind::Class: detail = "class"; break;
		default: detail = symbol.detail; break;
	}
	if (!symbol.declared_type.empty() && !callable_kind(symbol.kind) && symbol.kind != SymbolKind::Enum &&
			symbol.kind != SymbolKind::Class) detail += ": " + symbol.declared_type;
	return completion_item(symbol.name, std::move(detail), symbol.documentation, symbol.kind, has_arguments, symbol.id);
}

CompletionItem completion_item(const NativeMember &member) {
	auto has_arguments = member.signature &&
		(!member.signature->arguments.empty() || member.signature->is_vararg);
	std::string detail;
	if (member.signature) detail = member.is_static ? "static func" : "func";
	else if (member.kind == SymbolKind::Enum) detail = "enum";
	else if (member.kind == SymbolKind::Constant) detail = "const";
	else detail = member.is_static ? "static var" : "var";
	if (!member.type.empty() && !member.signature && member.kind != SymbolKind::Enum) detail += ": " + member.type;
	return completion_item(member.name, std::move(detail), member.documentation, member.kind, has_arguments,
		"native:" + member.owner + "::" + member.name);
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

void collect_identifiers(const Document &document, const SyntaxNode &node,
		std::unordered_set<std::string> &identifiers) {
	if (node.kind == "identifier") identifiers.insert(std::string(document.text(node)));
	for (const auto &child : node.children) collect_identifiers(document, child, identifiers);
}

std::vector<std::string> quoted_script_paths(std::string_view source) {
	std::vector<std::string> result;
	for (size_t index = 0; index < source.size();) {
		auto quote = source[index];
		if (quote != '\'' && quote != '"') {
			++index;
			continue;
		}
		auto begin = ++index;
		bool escaped = false;
		while (index < source.size()) {
			auto character = source[index];
			if (escaped) escaped = false;
			else if (character == '\\') escaped = true;
			else if (character == quote) break;
			++index;
		}
		if (index >= source.size()) break;
		auto value = std::string(source.substr(begin, index - begin));
		if (value.starts_with("res://") || value.starts_with("uid://") || value.ends_with(".gd")) {
			result.push_back(std::move(value));
		}
		++index;
	}
	return result;
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
	unsafe_call_argument_ = WarningLevel::Ignore;
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
	uid_paths_.clear();
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
	unsafe_call_argument_ = WarningLevel::Ignore;
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
			else if (name == "gdscript/warnings/unsafe_call_argument") unsafe_call_argument_ = warning_level;
		}
	}
}

void Workspace::rebuild_registry() {
	classes_.clear();
	global_classes_.clear();
	global_name_counts_.clear();
	symbol_owners_.clear();
	symbols_.clear();
	{
		std::lock_guard cache_lock(access_path_cache_mutex_);
		access_path_cache_.clear();
	}
	static_symbol_types_.clear();
	document_dependencies_.clear();
	reverse_document_dependencies_.clear();
	for (auto &[uri, document] : documents_) {
		(void)uri;
		for (auto &record : document->classes()) {
			classes_[record.symbol.id] = &record;
			symbols_[record.symbol.id] = &record.symbol;
			if (!record.global_name.empty()) ++global_name_counts_[record.global_name];
			std::function<void(const Symbol &, const std::string &)> register_symbol =
					[&](const Symbol &symbol, const std::string &owner) {
				symbols_[symbol.id] = &symbol;
				symbol_owners_[symbol.id] = owner;
				for (const auto &child : symbol.children) register_symbol(child, symbol.id);
			};
			for (const auto &member : record.members) register_symbol(member, record.symbol.id);
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

	// Build a conservative document graph from every resolved structural edge
	// and every source-level global/script reference. The identifier index also
	// preserves invalidation when a formerly unresolved global becomes valid.
	std::unordered_map<std::string, std::string> resource_uris;
	for (const auto &[uri, document] : documents_) resource_uris[document->resource_path()] = uri;
	auto dependency_uri = [&](std::string resource) -> std::string {
		if (auto found = resource_uris.find(resource); found != resource_uris.end()) return found->second;
		// Inner class IDs append a dotted suffix to the owning .gd resource.
		auto script_end = resource.find(".gd.");
		if (script_end != std::string::npos) {
			resource.resize(script_end + 3);
			if (auto found = resource_uris.find(resource); found != resource_uris.end()) return found->second;
		}
		return {};
	};
	for (const auto &[uri, document] : documents_) {
		auto &dependencies = document_dependencies_[uri];
		auto add_dependency = [&](const std::string &target) {
			if (!target.empty() && target != uri) dependencies.insert(target);
		};
		for (const auto &record : document->classes()) {
			if (!record.base_class_id.empty() && !record.base_class_id.starts_with("native:")) {
				if (auto *base = find_class(record.base_class_id)) add_dependency(base->symbol.uri);
				else add_dependency(dependency_uri(record.base_class_id));
			}
		}
		std::unordered_set<std::string> identifiers;
		collect_identifiers(*document, document->syntax_root(), identifiers);
		for (const auto &identifier : identifiers) {
			if (auto global = global_classes_.find(identifier); global != global_classes_.end()) {
				if (auto *record = find_class(global->second)) add_dependency(record->symbol.uri);
			}
			if (auto autoload = autoloads_.find(identifier); autoload != autoloads_.end()) {
				add_dependency(dependency_uri(resolve_path_reference(autoload->second, document->resource_path())));
			}
		}
		for (auto path : quoted_script_paths(document->source())) {
			add_dependency(dependency_uri(resolve_path_reference(std::move(path), document->resource_path())));
		}
	}
	for (const auto &[dependent, dependencies] : document_dependencies_) {
		for (const auto &dependency : dependencies) reverse_document_dependencies_[dependency].insert(dependent);
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
	auto path = path_for_uri(uri);
	if (!std::filesystem::exists(path)) {
		documents_.erase(uri);
		disk_sources_.erase(found);
		rebuild_registry();
		return true;
	}
	auto source = read_file(path);
	if (source.empty() && std::filesystem::file_size(path) != 0) {
		if (error) *error = "cannot read file";
		return false;
	}
	found->second = source;
	documents_[uri] = std::make_shared<Document>(uri, resource_path(path), std::move(source));
	rebuild_registry();
	return true;
}

bool Workspace::refresh_file(const std::string &uri, std::string *error) {
	std::unique_lock lock(mutex_);
	auto path = path_for_uri(uri);
	if (path.string().ends_with(".gd.uid")) {
		scan_uid_files();
		rebuild_registry();
		return true;
	}
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
	if (result.known()) result.declaration_id = symbol.id;
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
		return type_for_resource_path(std::move(resolved), context);
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
			const Symbol *found = nullptr;
			for (const auto &[id, record] : classes_) {
				(void)id;
				for (const auto &member : record->members) {
					if (member.id != current.symbol_id) continue;
					auto value = std::find_if(member.children.begin(), member.children.end(),
						[&](const Symbol &entry) { return entry.name == part; });
					if (value != member.children.end()) found = &*value;
					break;
				}
				if (found) break;
			}
			if (!found) return ResolvedType::unknown(expression);
			// GDScript enum values retain the enum datatype. This is what lets an
			// inferred local continue to drive enum-aware completion.
			current.declaration_id = found->id;
		} else {
			return ResolvedType::unknown(expression);
		}
		if (!current.known()) return ResolvedType::unknown(expression);
	}
	return current;
}

std::vector<const Symbol *> Workspace::all_members(const ClassRecord &record, MemberAccess access) const {
	std::vector<const Symbol *> result;
	std::unordered_set<std::string> names;
	std::unordered_set<std::string> classes_seen;
	auto *current = &record;
	while (current && classes_seen.insert(current->symbol.id).second) {
		auto append = [&](bool type_level) {
			for (const auto &member : current->members) {
				auto is_type_level = member.is_static || member.kind == SymbolKind::Constant ||
					member.kind == SymbolKind::Enum || member.kind == SymbolKind::Class;
				if (is_type_level == type_level && names.insert(member.name).second) result.push_back(&member);
			}
		};
		if (access == MemberAccess::Instance) {
			append(false);
			append(true);
		} else {
			append(true);
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

const ClassRecord *Workspace::enclosing_class(const ClassRecord &record) const {
	auto script_end = record.symbol.id.find(".gd");
	auto separator = record.symbol.id.rfind('.');
	if (script_end == std::string::npos || separator == std::string::npos || separator <= script_end + 2) return nullptr;
	return find_class(record.symbol.id.substr(0, separator));
}

const Symbol *Workspace::find_lexical_member(const ClassRecord &record, std::string_view name) const {
	for (auto *scope = enclosing_class(record); scope; scope = enclosing_class(*scope)) {
		for (const auto &member : scope->members) {
			if (member.name != name) continue;
			if (member.kind == SymbolKind::Constant || member.kind == SymbolKind::Enum ||
					member.kind == SymbolKind::Class || member.is_static) return &member;
		}
	}
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
		if (auto *record = find_class(name)) return {TypeKind::ScriptClass, record->symbol.name, name, true};
		return type_for_resource_path(name, context);
	}
	if (global_classes_.contains(name)) return {TypeKind::ScriptClass, name, global_classes_.at(name), true};
	if (context && classes_.contains(context->symbol.id + "." + name)) {
		return {TypeKind::ScriptClass, name, context->symbol.id + "." + name, true};
	}
	if (context) {
		std::unordered_set<std::string> stack;
		auto resolved = resolve_static_reference(name, context, stack);
		// A type annotation accepts metatype constants (preloaded scripts, class
		// aliases, builtin/native aliases) and enums. Ordinary value constants are
		// deliberately not types, even though their value type is known.
		if (resolved.known() && (resolved.kind == TypeKind::Enum ||
				(!resolved.instance && (resolved.kind == TypeKind::ScriptClass ||
				 resolved.kind == TypeKind::NativeClass || resolved.kind == TypeKind::Builtin)))) {
			if (resolved.kind != TypeKind::Enum) resolved.instance = true;
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

std::optional<std::string> Workspace::invalid_type_message(std::string_view name,
		const ClassRecord *context) const {
	if (!context || name.empty()) return std::nullopt;
	std::unordered_set<std::string> stack;
	auto resolved = resolve_static_reference(std::string(name), context, stack);
	if (!resolved.known()) return std::nullopt;
	if (resolved.kind == TypeKind::Enum || (!resolved.instance &&
			(resolved.kind == TypeKind::ScriptClass || resolved.kind == TypeKind::NativeClass ||
			 resolved.kind == TypeKind::Builtin))) return std::nullopt;
	auto dot = name.rfind('.');
	if (dot != std::string_view::npos) {
		return "Member \"" + std::string(name.substr(dot + 1)) + "\" under base \"" +
			std::string(name.substr(0, dot)) + "\" is not a valid type.";
	}
	if (auto *symbol = find_member(*context, name); symbol && symbol->kind == SymbolKind::Constant) {
		return "\"" + std::string(name) + "\" is a constant but does not contain a type.";
	}
	return "\"" + std::string(name) + "\" is not a valid type.";
}

ResolvedType Workspace::type_for_resource_path(std::string resource, const ClassRecord *context) const {
	resource = resolve_path_reference(std::move(resource), context ? context->symbol.id : "res://");
	auto extension = std::filesystem::path(resource).extension().string();
	if (extension == ".gd") {
		auto *record = find_class(resource);
		return {TypeKind::ScriptClass, record ? record->symbol.name : resource, resource, false};
	}
	if (extension == ".tscn" || extension == ".scn") {
		return {TypeKind::NativeClass, "PackedScene", "native:PackedScene", true};
	}
	if (!extension.empty() && native_api_.has_class("Resource")) {
		return {TypeKind::NativeClass, "Resource", "native:Resource", true};
	}
	return ResolvedType::unknown(resource);
}

const Symbol *Workspace::resolve_identifier(const Document &document, const ClassRecord *context,
		std::string_view name, Position position) const {
	if (auto *local = document.find_local(name, position)) return local;
	if (context) {
		if (auto *member = find_member(*context, name)) return member;
		if (auto *member = find_lexical_member(*context, name)) return member;
	}
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
	if (symbol.kind == SymbolKind::Event) {
		result = {TypeKind::Signal, "Signal", symbol.id};
		for (const auto &parameter : symbol.children) if (parameter.is_parameter) {
			auto argument = type_from_name(parameter.declared_type, context);
			result.signal_arguments.push_back(argument.known() ? argument : ResolvedType{TypeKind::Variant, "Variant"});
		}
	}
	else if (symbol.kind == SymbolKind::Method || symbol.kind == SymbolKind::Function ||
			symbol.kind == SymbolKind::Constructor) {
		result = {TypeKind::Callable, "Callable", symbol.id};
		result.callable_return = std::make_shared<ResolvedType>(callable_return_type(symbol, *declaration_document, stack));
	}
	else if (!symbol.declared_type.empty()) result = type_from_name(symbol.declared_type, context);
	else if (symbol.kind == SymbolKind::Enum) result = {TypeKind::Enum, symbol.name, symbol.id};
	else if (!symbol.initializer.empty() && (symbol.kind == SymbolKind::Constant || symbol.is_inferred)) result = infer_expression(symbol.initializer, *declaration_document, context,
		declaration_document == &document ? position : symbol.range.start, stack);
	else if (symbol.is_iteration_variable && !symbol.initializer.empty()) {
		result = iterable_value_type(infer_expression(symbol.initializer, *declaration_document, context,
			symbol.range.start, stack));
	}
	else result = {TypeKind::Variant, "Variant"};
	stack.pop_back();
	if (result.known()) result.declaration_id = symbol.id;
	return result;
}

ResolvedType Workspace::hinted_type_of_symbol(const Symbol &symbol, const Document &document, Position position,
		std::vector<std::string> &stack) const {
	auto result = type_of_symbol(symbol, document, position, stack);
	if (result.kind != TypeKind::Variant || !symbol.declared_type.empty() || symbol.initializer.empty() ||
			symbol.is_parameter || (symbol.kind != SymbolKind::Variable && symbol.kind != SymbolKind::Constant)) {
		return result;
	}
	if (std::find(stack.begin(), stack.end(), symbol.id) != stack.end()) return result;
	stack.push_back(symbol.id);
	auto *declaration_document = find_document(symbol.uri);
	if (!declaration_document) declaration_document = &document;
	auto hint = infer_expression(symbol.initializer, *declaration_document,
		declaration_document->class_at(symbol.range.start), symbol.range.start, stack);
	stack.pop_back();
	return hint.known() && hint.kind != TypeKind::Void ? hint : result;
}

ResolvedType Workspace::callable_return_type(const Symbol &symbol, const Document &document,
		std::vector<std::string> &stack) const {
	if (symbol.kind == SymbolKind::Constructor) {
		auto owner = symbol_owners_.find(symbol.id);
		if (owner != symbol_owners_.end()) {
			if (auto *record = find_class(owner->second)) {
				return {TypeKind::ScriptClass, record->symbol.name, record->symbol.id, true};
			}
		}
	}
	if (symbol.kind != SymbolKind::Method && symbol.kind != SymbolKind::Function) {
		return {TypeKind::Variant, "Variant"};
	}
	auto *declaration_document = find_document(symbol.uri);
	if (!declaration_document) declaration_document = &document;
	auto *declaration_context = declaration_document->class_at(symbol.range.start);
	if (!symbol.declared_type.empty()) {
		auto result = type_from_name(symbol.declared_type, declaration_context);
		return result.known() ? result : ResolvedType{TypeKind::Variant, "Variant"};
	}

	auto marker = "return:" + symbol.id;
	if (std::find(stack.begin(), stack.end(), marker) != stack.end()) return {TypeKind::Variant, "Variant"};
	stack.push_back(marker);
	const SyntaxNode *function = nullptr;
	std::function<void(const SyntaxNode &)> find_function = [&](const SyntaxNode &node) {
		if (function) return;
		if ((node.kind == "function_definition" || node.kind == "constructor_definition") &&
				node.range.start == symbol.range.start) {
			function = &node;
			return;
		}
		for (const auto &child : node.children) find_function(child);
	};
	find_function(declaration_document->syntax_root());
	std::vector<ResolvedType> returns;
	if (function) {
		std::function<void(const SyntaxNode &, bool)> collect_returns = [&](const SyntaxNode &node, bool root) {
			if (!root && (node.kind == "function_definition" || node.kind == "constructor_definition" ||
					node.kind == "lambda" || node.kind == "class_definition")) return;
			if (node.kind == "return_statement") {
				if (!node.children.empty()) {
					auto expression = std::string(declaration_document->text(node.children.front()));
					returns.push_back(infer_expression(std::move(expression), *declaration_document,
						declaration_context, node.children.front().range.start, stack));
				}
				return;
			}
			for (const auto &child : node.children) collect_returns(child, false);
		};
		collect_returns(*function, true);
	}
	stack.pop_back();
	if (returns.empty()) return {TypeKind::Variant, "Variant"};
	auto result = returns.front();
	for (size_t index = 1; index < returns.size(); ++index) {
		const auto &other = returns[index];
		if (result.kind != other.kind || result.name != other.name || result.symbol_id != other.symbol_id ||
				result.instance != other.instance || result.arguments.size() != other.arguments.size()) {
			return {TypeKind::Variant, "Variant"};
		}
	}
	return result.known() ? result : ResolvedType{TypeKind::Variant, "Variant"};
}

ResolvedType Workspace::member_value_type(const ResolvedType &receiver, std::string_view member_name,
		const Document &document, Position position, std::vector<std::string> &stack) const {
	if (!receiver.instance && member_name == "new" && (receiver.kind == TypeKind::ScriptClass ||
			receiver.kind == TypeKind::NativeClass || receiver.kind == TypeKind::Builtin)) {
		auto callable_id = receiver.symbol_id + "::new";
		if (receiver.kind == TypeKind::ScriptClass) {
			if (auto *record = find_class(receiver.symbol_id)) {
				if (auto *constructor = find_member(*record, "_init")) callable_id = constructor->id;
			}
		}
		auto result = ResolvedType{TypeKind::Callable, "Callable", std::move(callable_id)};
		result.declaration_id = result.symbol_id;
		auto instance = receiver;
		instance.instance = true;
		result.callable_return = std::make_shared<ResolvedType>(std::move(instance));
		return result;
	}
	if (receiver.kind == TypeKind::ScriptClass) {
		if (auto *record = find_class(receiver.symbol_id)) {
			if (auto *member = find_member(*record, member_name)) {
				if (!receiver.instance) {
					auto type_level = member->is_static || member->kind == SymbolKind::Constant ||
						member->kind == SymbolKind::Enum || member->kind == SymbolKind::Class;
					if (!type_level) return ResolvedType::unknown(std::string(member_name));
					if (member->kind == SymbolKind::Constant || member->kind == SymbolKind::Enum ||
							member->kind == SymbolKind::Class) {
						std::unordered_set<std::string> static_stack;
						auto resolved = resolve_static_symbol(*member, static_stack);
						if (resolved.known()) {
							resolved.declaration_id = member->id;
							return resolved;
						}
					}
				}
				if (member->kind == SymbolKind::Method || member->kind == SymbolKind::Function ||
						member->kind == SymbolKind::Constructor) {
					auto result = ResolvedType{TypeKind::Callable, "Callable", member->id};
					result.declaration_id = member->id;
					result.callable_return = std::make_shared<ResolvedType>(callable_return_type(*member, document, stack));
					return result;
				}
				auto result = hinted_type_of_symbol(*member, document, position, stack);
				result.declaration_id = member->id;
				return result;
			}
			auto native = receiver.instance ? native_base(*record) :
				(native_api_.has_class("GDScript") ? std::string("GDScript") : std::string("Script"));
			if (!native.empty()) {
				if (auto *member = native_api_.find_member(native, member_name)) {
					if (member->kind == SymbolKind::Event) {
						ResolvedType result{TypeKind::Signal, "Signal", "native:" + member->owner + "::" + member->name};
						result.declaration_id = result.symbol_id;
						if (member->signature) for (const auto &argument : member->signature->arguments) {
							auto type = type_from_name(argument.type, record);
							result.signal_arguments.push_back(type.known() ? type : ResolvedType{TypeKind::Variant, "Variant"});
						}
						return result;
					}
					if (member->signature) {
						ResolvedType result{TypeKind::Callable, "Callable", "native:" + member->owner + "::" + member->name};
						result.declaration_id = result.symbol_id;
						auto returned = type_from_name(member->signature->return_type, record);
						result.callable_return = std::make_shared<ResolvedType>(returned.known() ? returned : ResolvedType{TypeKind::Variant, "Variant"});
						return result;
					}
					auto result = type_from_name(member->type, record);
					result.declaration_id = "native:" + member->owner + "::" + member->name;
					return result.known() ? result : ResolvedType{TypeKind::Variant, "Variant"};
				}
			}
		}
	} else if (receiver.kind == TypeKind::NativeClass || receiver.kind == TypeKind::Builtin ||
			receiver.kind == TypeKind::Callable || receiver.kind == TypeKind::Signal) {
		if (receiver.kind == TypeKind::Callable && (member_name == "bind" || member_name == "bindv" ||
				member_name == "unbind")) return receiver;
		if (auto *member = native_api_.find_member(receiver.name, member_name)) {
			if (member->kind == SymbolKind::Enum) {
				auto result = ResolvedType{TypeKind::Enum, member->name,
					"nativeenum:" + member->owner + "." + member->name, false};
				result.declaration_id = "native:" + member->owner + "::" + member->name;
				return result;
			}
			if (member->kind == SymbolKind::Event) {
				ResolvedType result{TypeKind::Signal, "Signal", "native:" + member->owner + "::" + member->name};
				result.declaration_id = result.symbol_id;
				if (member->signature) for (const auto &argument : member->signature->arguments) {
					auto type = type_from_name(argument.type, nullptr);
					result.signal_arguments.push_back(type.known() ? type : ResolvedType{TypeKind::Variant, "Variant"});
				}
				return result;
			}
			if (member->signature) {
				ResolvedType result{TypeKind::Callable, "Callable", "native:" + member->owner + "::" + member->name};
				result.declaration_id = result.symbol_id;
				auto returned = type_from_name(member->signature->return_type, nullptr);
				result.callable_return = std::make_shared<ResolvedType>(returned.known() ? returned : ResolvedType{TypeKind::Variant, "Variant"});
				return result;
			}
			auto result = type_from_name(member->type, nullptr);
			result.declaration_id = "native:" + member->owner + "::" + member->name;
			return result.known() ? result : ResolvedType{TypeKind::Variant, "Variant"};
		}
	} else if (receiver.kind == TypeKind::Enum) {
		auto result = receiver;
		if (receiver.symbol_id.starts_with("nativeenum:")) {
			auto qualified = receiver.symbol_id.substr(11);
			auto separator = qualified.rfind('.');
			result.declaration_id = "native:" + qualified.substr(0, separator) + "::" + std::string(member_name);
		} else if (receiver.symbol_id.starts_with("global:")) {
			result.declaration_id = receiver.symbol_id + "::" + std::string(member_name);
		} else {
			result.declaration_id = receiver.symbol_id + "." + std::string(member_name);
		}
		return result;
	}
	return ResolvedType::unknown(std::string(member_name));
}

ResolvedType Workspace::infer_expression(std::string expression, const Document &document, const ClassRecord *context,
		Position position, std::vector<std::string> &stack) const {
	expression = trim(expression);
	while (expression.size() >= 2 && expression.front() == '(' && expression.back() == ')') {
		expression = trim(expression.substr(1, expression.size() - 2));
	}
	if (expression.empty()) return ResolvedType::unknown();
	if (expression.starts_with("await ")) {
		auto awaited = infer_expression(expression.substr(6), document, context, position, stack);
		if (awaited.kind != TypeKind::Signal || awaited.signal_arguments.empty()) {
			return {TypeKind::Variant, "Variant"};
		}
		if (awaited.signal_arguments.size() == 1) return awaited.signal_arguments.front();
		ResolvedType result{TypeKind::Builtin, "Array"};
		result.arguments.push_back({TypeKind::Variant, "Variant"});
		return result;
	}
	if (expression.starts_with("func") && expression.find('(') != std::string::npos) {
		return {TypeKind::Callable, "Callable", "lambda"};
	}
	if (expression == "true" || expression == "false") return {TypeKind::Builtin, "bool"};
	if (expression == "null") return {TypeKind::Variant, "Variant"};
	if (expression.front() == '"' || expression.front() == '\'' || expression.starts_with("&\"") || expression.starts_with("&'")) {
		return {TypeKind::Builtin, expression.front() == '&' ? "StringName" : "String"};
	}
	if (expression.front() == '[') return {TypeKind::Builtin, "Array"};
	if (expression.front() == '{') return {TypeKind::Builtin, "Dictionary"};
	if (is_integer_literal(expression)) return {TypeKind::Builtin, "int"};
	if (is_float_literal(expression)) return {TypeKind::Builtin, "float"};
	if (expression == "self" && context) return {TypeKind::ScriptClass, context->symbol.name, context->symbol.id, true};
	if (expression == "super" && context && !context->base_class_id.empty()) {
		if (context->base_class_id.starts_with("native:")) {
			return {TypeKind::NativeClass, context->base_class_id.substr(7), context->base_class_id, true};
		}
		if (auto *base = find_class(context->base_class_id)) {
			return {TypeKind::ScriptClass, base->symbol.name, base->symbol.id, true};
		}
	}
	for (auto loader : {std::string_view("preload"), std::string_view("load")}) {
		if (!expression.starts_with(loader)) continue;
		auto call = trim(std::string_view(expression).substr(loader.size()));
		if (call.size() < 4 || call.front() != '(' || call.back() != ')') continue;
		auto argument = trim(std::string_view(call).substr(1, call.size() - 2));
		if (argument.size() < 2 || (argument.front() != '"' && argument.front() != '\'') || argument.back() != argument.front()) continue;
		auto id = resolve_path_reference(argument, document.resource_path());
		return type_for_resource_path(id, context);
	}
	if (auto callee = terminal_call(expression)) {
		if (auto member = trailing_member(callee->callee)) {
			auto &receiver_text = member->first;
			auto &member_name = member->second;
			auto receiver = infer_expression(receiver_text, document, context, position, stack);
			if (!receiver.known()) {
				std::unordered_set<std::string> static_stack;
				receiver = resolve_static_reference(receiver_text, context, static_stack);
			}
			if (receiver.kind == TypeKind::Builtin && receiver.name == "Dictionary") {
				if (member_name == "keys") {
					ResolvedType result{TypeKind::Builtin, "Array"};
					if (!receiver.arguments.empty()) result.arguments.push_back(receiver.arguments.front());
					result.declaration_id = "native:Dictionary::keys";
					return result;
				}
				if (member_name == "values") {
					ResolvedType result{TypeKind::Builtin, "Array"};
					if (receiver.arguments.size() > 1) result.arguments.push_back(receiver.arguments[1]);
					result.declaration_id = "native:Dictionary::values";
					return result;
				}
				if (member_name == "get" && receiver.arguments.size() > 1) return receiver.arguments[1];
			}
			if (receiver.kind == TypeKind::Callable && (member_name == "call" || member_name == "callv")) {
				return receiver.callable_return ? *receiver.callable_return : ResolvedType{TypeKind::Variant, "Variant"};
			}
			if (receiver.kind == TypeKind::Callable && (member_name == "bind" || member_name == "bindv" ||
					member_name == "unbind")) return receiver;
			if ((receiver.kind == TypeKind::ScriptClass || receiver.kind == TypeKind::NativeClass) &&
					member_name == "get") {
				if (auto property_name = quoted_value(callee->arguments)) {
					auto property = member_value_type(receiver, *property_name, document, position, stack);
					if (property.known()) return property;
				}
			}
			auto callable = member_value_type(receiver, member_name, document, position, stack);
			if (callable.kind == TypeKind::Callable && callable.callable_return) {
				auto returned = *callable.callable_return;
				returned.declaration_id = callable.declaration_id.empty() ? callable.symbol_id : callable.declaration_id;
				return returned;
			}
			return callable.kind == TypeKind::Callable ? ResolvedType{TypeKind::Variant, "Variant"} :
				ResolvedType::unknown(member_name);
		} else {
			if (callee->callee == "new" && context) {
				return {TypeKind::ScriptClass, context->symbol.name, context->symbol.id, true};
			}
			if (auto *signature = native_api_.find_utility_function(callee->callee)) {
				auto result = type_from_name(signature->return_type, context);
				result.declaration_id = "utility:" + callee->callee;
				return result.known() ? result : ResolvedType{TypeKind::Variant, "Variant"};
			}
			if (auto *function = find_gdscript_builtin_function(callee->callee)) {
				auto result = type_from_name(function->signature.return_type, context);
				result.declaration_id = "builtin:" + callee->callee;
				return result.known() ? result : ResolvedType{TypeKind::Variant, "Variant"};
			}
			auto callable = infer_expression(callee->callee, document, context, position, stack);
			if (callable.kind == TypeKind::Callable) {
				if (!callable.callable_return) return {TypeKind::Variant, "Variant"};
				auto returned = *callable.callable_return;
				returned.declaration_id = callable.declaration_id.empty() ? callable.symbol_id : callable.declaration_id;
				return returned;
			}
			auto constructed = type_from_name(callee->callee, context);
			if (constructed.kind == TypeKind::Builtin || constructed.kind == TypeKind::NativeClass ||
					constructed.kind == TypeKind::ScriptClass) {
				constructed.instance = true;
				return constructed;
			}
		}
	}
	if (auto member = trailing_member(expression)) {
		auto receiver = infer_expression(member->first, document, context, position, stack);
		if (!receiver.known()) {
			std::unordered_set<std::string> static_stack;
			receiver = resolve_static_reference(member->first, context, static_stack);
		}
		return member_value_type(receiver, member->second, document, position, stack);
	}
	if (auto subscript = terminal_subscript(expression)) {
		auto receiver = infer_expression(subscript->first, document, context, position, stack);
		if (auto name = quoted_value(subscript->second);
				name && (receiver.kind == TypeKind::ScriptClass || receiver.kind == TypeKind::NativeClass ||
					receiver.kind == TypeKind::Builtin)) {
			auto member = member_value_type(receiver, *name, document, position, stack);
			if (member.known()) return member;
		}
		if (receiver.kind == TypeKind::Builtin && receiver.name == "Array" && !receiver.arguments.empty()) {
			return receiver.arguments.front();
		}
		if (receiver.kind == TypeKind::Builtin && receiver.name == "Dictionary" && receiver.arguments.size() > 1) {
			return receiver.arguments[1];
		}
		if (receiver.kind == TypeKind::Builtin && receiver.name == "String") return receiver;
		if (receiver.kind == TypeKind::Builtin && receiver.name == "Color") return {TypeKind::Builtin, "float"};
		auto element = iterable_value_type(receiver);
		if (element.kind != TypeKind::Variant) return element;
		return {TypeKind::Variant, "Variant"};
	}
	if (is_identifier(expression)) {
		if (auto found = autoloads_.find(expression); found != autoloads_.end()) {
			auto result = type_from_name(resolve_path_reference(found->second, document.resource_path()), context);
			result.instance = true;
			return result;
		}
		if (auto singleton = native_api_.singleton_type(expression)) {
			auto result = type_from_name(*singleton, context);
			result.instance = true;
			return result;
		}
		if (auto *symbol = resolve_identifier(document, context, expression, position)) {
			auto result = hinted_type_of_symbol(*symbol, document, position, stack);
			result.declaration_id = symbol->id;
			return result;
		}
		if (auto *signature = native_api_.find_utility_function(expression)) {
			ResolvedType result{TypeKind::Callable, "Callable", "utility:" + expression};
			result.declaration_id = result.symbol_id;
			auto returned = type_from_name(signature->return_type, context);
			result.callable_return = std::make_shared<ResolvedType>(returned.known() ? returned : ResolvedType{TypeKind::Variant, "Variant"});
			return result;
		}
		if (auto *function = find_gdscript_builtin_function(expression)) {
			ResolvedType result{TypeKind::Callable, "Callable", "builtin:" + expression};
			result.declaration_id = result.symbol_id;
			auto returned = type_from_name(function->signature.return_type, context);
			result.callable_return = std::make_shared<ResolvedType>(returned.known() ? returned : ResolvedType{TypeKind::Variant, "Variant"});
			return result;
		}
		auto result = type_from_name(expression, context);
		if (result.kind == TypeKind::ScriptClass || result.kind == TypeKind::NativeClass ||
				result.kind == TypeKind::Builtin) result.instance = false;
		return result;
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
	return resolve_expression(uri, position, std::move(expression)).type;
}

std::optional<SymbolOrigin> Workspace::symbol_origin(std::string_view id) const {
	if (id.empty()) return std::nullopt;
	if (id.starts_with("native:")) {
		auto separator = id.rfind("::");
		if (separator == std::string_view::npos) {
			auto name = std::string(id.substr(7));
			return SymbolOrigin{std::string(id), {}, "native", name, SymbolKind::Class, {}, true};
		}
		auto owner = std::string(id.substr(7, separator - 7));
		auto name = std::string(id.substr(separator + 2));
		if (auto *member = native_api_.find_member(owner, name)) {
			return SymbolOrigin{std::string(id), {}, "native:" + member->owner, member->name,
				member->kind, {}, true};
		}
		return SymbolOrigin{std::string(id), {}, "native:" + owner, name, SymbolKind::Method, {}, true};
	}
	if (id.starts_with("utility:") || id.starts_with("builtin:")) {
		auto separator = id.find(':');
		return SymbolOrigin{std::string(id), {}, std::string(id.substr(0, separator)),
			std::string(id.substr(separator + 1)), SymbolKind::Function, {}, true};
	}
	if (auto found = symbols_.find(std::string(id)); found != symbols_.end()) {
		auto *symbol = found->second;
		auto owner = symbol_owners_.find(symbol->id);
		return SymbolOrigin{symbol->id, symbol->uri,
			owner == symbol_owners_.end() ? std::string{} : owner->second,
			symbol->name, symbol->kind, symbol->selection_range, true};
	}
	return std::nullopt;
}

std::string Workspace::expression_type_access(std::string expression, const ResolvedType &type,
		const Document &document, const ClassRecord *context, Position position, size_t depth) const {
	if (depth >= 8) return {};
	expression = trim(expression);
	if (expression.empty()) return {};
	auto same_type = [&](const ResolvedType &candidate) {
		return candidate.kind == type.kind && candidate.symbol_id == type.symbol_id &&
			(!candidate.symbol_id.empty() || candidate.name == type.name);
	};
	// Static enum values resolve to their enum type too. Prefer the enclosing
	// enum access (`State`) over the value expression (`State.IDLE`).
	if (type.kind == TypeKind::Enum) {
		auto candidate = expression;
		for (size_t attempts = 0; attempts < 16; ++attempts) {
			auto separator = candidate.rfind('.');
			if (separator == std::string::npos) break;
			candidate = trim(candidate.substr(0, separator));
			if (same_type(type_from_name(candidate, context))) return candidate;
		}
	}
	if (same_type(type_from_name(expression, context))) return expression;

	auto identifier = expression.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
		std::string::npos;
	if (identifier) {
		if (auto *symbol = resolve_identifier(document, context, expression, position)) {
			auto owner = symbol_owners_.find(symbol->id);
			auto *declaration_context = owner == symbol_owners_.end() ? context : find_class(owner->second);
			if (!symbol->declared_type.empty() && same_type(type_from_name(symbol->declared_type, declaration_context))) {
				return symbol->declared_type;
			}
			if (!symbol->initializer.empty()) {
				return expression_type_access(symbol->initializer, type, document, context, position, depth + 1);
			}
		}
	}

	// Constructors spell the instance type immediately before `.new`. Enum
	// values similarly spell their enum type before the final member.
	if (auto new_at = expression.find(".new("); new_at != std::string::npos) {
		auto candidate = trim(expression.substr(0, new_at));
		if (same_type(type_from_name(candidate, context))) return candidate;
	}
	auto candidate = expression;
	for (size_t attempts = 0; attempts < 16; ++attempts) {
		auto separator = candidate.rfind('.');
		if (separator == std::string::npos) break;
		candidate = trim(candidate.substr(0, separator));
		if (same_type(type_from_name(candidate, context))) return candidate;
	}
	return {};
}

AccessProvenance Workspace::access_provenance(std::string expression, const ResolvedType &type,
		const Document &document, const ClassRecord *context, Position position, size_t depth) const {
	AccessProvenance result;
	if (depth >= 8 || !type.known()) return result;
	expression = trim(expression);
	auto identifier = expression.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
		std::string::npos;
	const Symbol *source_symbol = identifier ? resolve_identifier(document, context, expression, position) : nullptr;
	if (source_symbol) {
		auto owner = symbol_owners_.find(source_symbol->id);
		result.declaration_context_id = owner == symbol_owners_.end() ? std::string{} : owner->second;
		if (!source_symbol->declared_type.empty()) result.declaration_access = source_symbol->declared_type;
		if (result.declaration_access.empty() && !source_symbol->initializer.empty()) {
			result.declaration_access = expression_type_access(source_symbol->initializer, type,
				document, context, position, depth + 1);
			expression = source_symbol->initializer;
		}
	} else {
		result.declaration_access = expression_type_access(expression, type, document, context, position, depth + 1);
	}

	// If the value came from a call, retain the callable's declaration spelling
	// and translate it through the receiver spelling available to this caller.
	auto call_open = expression.rfind('(');
	if (call_open != std::string::npos) {
		auto callee_text = trim(expression.substr(0, call_open));
		std::vector<std::string> stack;
		auto callable = infer_expression(callee_text, document, context, position, stack);
		if (auto found = symbols_.find(callable.symbol_id); found != symbols_.end() &&
				callable_kind(found->second->kind) && !found->second->declared_type.empty()) {
			auto owner = symbol_owners_.find(found->second->id);
			result.declaration_context_id = owner == symbol_owners_.end() ? std::string{} : owner->second;
			result.declaration_access = found->second->declared_type;
			if (auto member = trailing_member(callee_text)) {
				std::vector<std::string> receiver_stack;
				auto receiver_type = infer_expression(member->first, document, context, position, receiver_stack);
				result.receiver_access = expression_type_access(member->first, receiver_type,
					document, context, position, depth + 1);
			}
		}
	}
	return result;
}

std::vector<AccessPath> Workspace::access_paths_for_type(const ResolvedType &type,
		const ClassRecord *context, const AccessProvenance &provenance) const {
	auto cache_key = (context ? context->symbol.id : std::string{}) + "\n" +
		std::to_string(static_cast<int>(type.kind)) + "\n" + type.name + "\n" + type.symbol_id + "\n" +
		provenance.declaration_access + "\n" + provenance.declaration_context_id + "\n" + provenance.receiver_access;
	{
		std::lock_guard cache_lock(access_path_cache_mutex_);
		if (auto found = access_path_cache_.find(cache_key); found != access_path_cache_.end()) return found->second;
	}
	std::vector<AccessPath> paths;
	std::set<std::string> seen;
	std::set<AccessPathKind> filled_kinds;
	auto same_type = [&](const ResolvedType &candidate) {
		return candidate.kind == type.kind && candidate.symbol_id == type.symbol_id &&
			(!candidate.symbol_id.empty() || candidate.name == type.name);
	};
	auto add = [&](std::string text, AccessPathKind kind, bool verify = true) {
		if (text.empty() || filled_kinds.contains(kind) || !seen.insert(text).second) return;
		if (verify && context && !same_type(type_from_name(text, context))) {
			seen.erase(text);
			return;
		}
		paths.push_back({std::move(text), kind, false});
		filled_kinds.insert(kind);
	};
	if (type.kind == TypeKind::Builtin || type.kind == TypeKind::Callable || type.kind == TypeKind::Signal) {
		add(type.name, AccessPathKind::Native, false);
	} else if (type.kind == TypeKind::NativeClass) {
		add(type.name, AccessPathKind::Native, false);
	} else if (type.kind == TypeKind::Enum && type.symbol_id.starts_with("nativeenum:")) {
		auto qualified = type.symbol_id.substr(11);
		auto separator = qualified.rfind('.');
		add(separator == std::string::npos ? qualified : qualified.substr(0, separator),
			AccessPathKind::Native, false);
	} else if (type.kind == TypeKind::Enum && type.symbol_id.starts_with("global:")) {
		add(type.symbol_id.substr(7), AccessPathKind::Global, false);
	} else if ((type.kind == TypeKind::ScriptClass || type.kind == TypeKind::Enum) && context) {
		auto enum_separator = type.symbol_id.rfind("::");
		std::string owner = type.kind == TypeKind::Enum && enum_separator != std::string::npos ?
			type.symbol_id.substr(0, enum_separator) : type.symbol_id;
		std::string terminal = type.kind == TypeKind::Enum && enum_separator != std::string::npos ?
			type.symbol_id.substr(enum_separator + 2) : std::string{};
		auto suffix_from = [&](std::string_view prefix) -> std::string {
			if (owner == prefix) return terminal;
			if (owner.starts_with(std::string(prefix) + ".")) {
				auto suffix = owner.substr(prefix.size() + 1);
				return terminal.empty() ? suffix : suffix + "." + terminal;
			}
			return {};
		};
		auto suffix_from_reachable_class = [&](std::string class_id) -> std::string {
			std::unordered_set<std::string> visited;
			while (!class_id.empty() && !class_id.starts_with("native:") && visited.insert(class_id).second) {
				auto suffix = suffix_from(class_id);
				if (!suffix.empty() || owner == class_id) return suffix;
				auto *record = find_class(class_id);
				class_id = record ? record->base_class_id : std::string{};
			}
			return {};
		};
		// Constants/classes visible from this scope are the most useful spelling:
		// retain one preload alias and one ordinary local spelling at most.
		std::vector<const Symbol *> visible = all_members(*context, MemberAccess::Type);
		for (auto *scope = enclosing_class(*context); scope; scope = enclosing_class(*scope)) {
			for (const auto &member : scope->members) if (member.kind == SymbolKind::Constant ||
					member.kind == SymbolKind::Enum || member.kind == SymbolKind::Class || member.is_static) {
				visible.push_back(&member);
			}
		}
		auto add_visible = [&](bool aliases) {
			for (auto *member : visible) {
				std::unordered_set<std::string> stack;
				auto reached = resolve_static_symbol(*member, stack);
				bool alias = member->kind == SymbolKind::Constant && reached.known() &&
					(reached.kind == TypeKind::Enum || !reached.instance);
				if (alias != aliases) continue;
				if (same_type(reached)) add(member->name, aliases ? AccessPathKind::ScriptAlias : AccessPathKind::Local);
				if (reached.kind == TypeKind::ScriptClass) {
					auto suffix = suffix_from_reachable_class(reached.symbol_id);
					if (!suffix.empty()) add(member->name + "." + suffix,
						aliases ? AccessPathKind::ScriptAlias : AccessPathKind::Local);
				}
			}
		};
		add_visible(true);

		// The spelling at the declaration site is authoritative. Translate a
		// relative spelling through the receiver used by the caller, then verify the
		// complete candidate resolves back to this exact symbol.
		auto candidate_kind = [&](std::string_view candidate) {
			auto separator = candidate.find('.');
			auto root = std::string(candidate.substr(0, separator));
			return global_classes_.contains(root) ? AccessPathKind::Global : AccessPathKind::Local;
		};
		if (!provenance.declaration_access.empty()) {
			add(provenance.declaration_access, candidate_kind(provenance.declaration_access));
			if (!provenance.receiver_access.empty()) {
				add(provenance.receiver_access + "." + provenance.declaration_access,
					candidate_kind(provenance.receiver_access));
			}
		}
		add_visible(false);

		// Relative spelling inside the declaring script/class.
		auto script_end = owner.find(".gd");
		if (script_end != std::string::npos) {
			auto root = owner.substr(0, script_end + 3);
			auto relative = suffix_from(root);
			if (!relative.empty()) add(relative, AccessPathKind::Local);
		}
		if (!terminal.empty()) add(terminal, AccessPathKind::Local);

		// A canonical global exists only when the target is lexically declared in
		// that global class's script. Do not manufacture aliases through every
		// global subclass or namespace graph.
		auto canonical_script_end = owner.find(".gd");
		if (canonical_script_end != std::string::npos) {
			auto root_id = owner.substr(0, canonical_script_end + 3);
			if (auto *root_record = find_class(root_id); root_record && !root_record->global_name.empty()) {
				auto suffix = suffix_from(root_id);
				if (owner == root_id && terminal.empty()) add(root_record->global_name, AccessPathKind::Global);
				else if (!suffix.empty()) add(root_record->global_name + "." + suffix, AccessPathKind::Global);
			}
		}
	}
	if (!paths.empty()) paths.front().preferred = true;
	{
		std::lock_guard cache_lock(access_path_cache_mutex_);
		access_path_cache_[std::move(cache_key)] = paths;
	}
	return paths;
}

ResolvedExpression Workspace::resolve_expression(const std::string &uri, Position position,
		std::string expression) const {
	std::shared_lock lock(mutex_);
	auto *document = find_document(uri);
	if (!document) return {ResolvedType::unknown("document not indexed"), std::nullopt, {}};
	if (expression.empty()) expression = identifier_at(document->source(), position);
	auto source_expression = expression;
	std::vector<std::string> stack;
	auto *context = document->class_at(position);
	auto type = infer_expression(std::move(expression), *document, context, position, stack);
	auto origin_id = type.declaration_id;
	if (origin_id.empty() && (type.kind == TypeKind::ScriptClass || type.kind == TypeKind::Enum ||
			type.kind == TypeKind::NativeClass)) origin_id = type.symbol_id;
	auto provenance = access_provenance(source_expression, type, *document, context, position);
	return {type, symbol_origin(origin_id), access_paths_for_type(type, context, provenance)};
}

std::optional<CompletionItem> Workspace::resolve_completion_item(std::string_view symbol_id) const {
	std::shared_lock lock(mutex_);
	if (symbol_id.starts_with("native:")) {
		auto separator = symbol_id.rfind("::");
		if (separator != std::string_view::npos) {
			auto owner = symbol_id.substr(7, separator - 7);
			auto name = symbol_id.substr(separator + 2);
			if (auto *member = native_api_.find_member(owner, name)) return completion_item(*member);
		}
	}
	if (auto found = symbols_.find(std::string(symbol_id)); found != symbols_.end()) {
		return completion_item(*found->second);
	}
	return std::nullopt;
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

bool Workspace::is_potential_downcast(const ResolvedType &expected, const ResolvedType &actual) const {
	if (!expected.known() || !actual.known() || expected.kind == TypeKind::Variant || actual.kind == TypeKind::Variant) return false;
	if (is_assignable(expected, actual)) return false;
	return is_assignable(actual, expected);
}

std::vector<CompletionItem> Workspace::semantic_completion_locked(const Document &document_value, Position position,
		const CaretContext &caret) const {
	std::vector<CompletionItem> result;
	auto *document = &document_value;
	std::set<std::string> names;
	auto add_symbol = [&](const Symbol &symbol) {
		if (!names.insert(symbol.name).second) return;
		result.push_back(completion_item(symbol));
	};
	if (caret.member_access) {
		if (!caret.member_receiver) return result;
		auto &receiver_text = *caret.member_receiver;
		std::vector<std::string> stack;
		auto *context = document->class_at(position);
		auto receiver = infer_expression(receiver_text, *document, context, position, stack);
		auto root_end = receiver_text.find('.');
		auto receiver_root = receiver_text.substr(0, root_end);
		auto *receiver_symbol = resolve_identifier(*document, context, receiver_root, position);
		auto singleton_type = root_end == std::string::npos ? native_api_.singleton_type(receiver_root) : std::nullopt;
		auto value_receiver = receiver_symbol || autoloads_.contains(receiver_root) || singleton_type.has_value();
		if (singleton_type) {
			receiver = type_from_name(*singleton_type, context);
			receiver.instance = true;
		}
		if (!value_receiver) {
			std::unordered_set<std::string> static_stack;
			auto static_receiver = resolve_static_reference(receiver_text, context, static_stack);
			if (static_receiver.known()) receiver = std::move(static_receiver);
		}
		if (receiver_symbol && !receiver_symbol->declared_type.empty() && receiver.kind != TypeKind::Enum) {
			receiver.instance = true;
		}
		if (receiver.kind == TypeKind::ScriptClass) {
			if (auto *record = find_class(receiver.symbol_id)) {
				for (auto *member : all_members(*record,
						receiver.instance ? MemberAccess::Instance : MemberAccess::Type)) add_symbol(*member);
				auto base = receiver.instance ? native_base(*record) :
					(native_api_.has_class("GDScript") ? std::string("GDScript") : std::string("Script"));
				if (!base.empty()) {
					for (auto *member : native_api_.members(base, MemberAccess::Instance)) {
						if (names.insert(member->name).second) {
							result.push_back(completion_item(*member));
						}
					}
				}
			}
		} else if (receiver.kind == TypeKind::NativeClass || receiver.kind == TypeKind::Builtin ||
				receiver.kind == TypeKind::Callable || receiver.kind == TypeKind::Signal) {
			auto native_name = receiver.kind == TypeKind::Callable ? std::string("Callable") :
				(receiver.kind == TypeKind::Signal ? std::string("Signal") : receiver.name);
			for (auto *member : native_api_.members(native_name,
					receiver.instance ? MemberAccess::Instance : MemberAccess::Type)) {
				if (names.insert(member->name).second) {
					result.push_back(completion_item(*member));
				}
			}
		} else if (receiver.kind == TypeKind::Enum) {
			for (const auto &[id, record] : classes_) {
				(void)id;
				for (const auto &member : record->members) if (member.id == receiver.symbol_id) {
					for (const auto &value : member.children) add_symbol(value);
				}
			}
			for (auto *member : native_api_.members("Dictionary")) {
				if (names.insert(member->name).second) {
					result.push_back(completion_item(*member));
				}
			}
		}
	} else {
		for (auto *local : document->locals_at(position)) add_symbol(*local);
		if (auto *record = document->class_at(position)) {
			bool static_context = false;
			for (const auto &member : record->members) {
				if ((member.kind == SymbolKind::Method || member.kind == SymbolKind::Constructor) &&
						member.range.contains(position)) {
					static_context = member.is_static;
					break;
				}
			}
			auto access = static_context ? MemberAccess::Type : MemberAccess::Instance;
			for (auto *member : all_members(*record, access)) add_symbol(*member);
			auto base = native_base(*record);
			if (!base.empty()) {
				for (auto *member : native_api_.members(base, access)) {
					if (names.insert(member->name).second) {
						result.push_back(completion_item(*member));
					}
				}
			}
			for (auto *scope = enclosing_class(*record); scope; scope = enclosing_class(*scope)) {
				for (const auto &member : scope->members) {
					if (member.kind == SymbolKind::Constant || member.kind == SymbolKind::Enum ||
							member.kind == SymbolKind::Class || member.is_static) add_symbol(member);
				}
			}
		}
		std::vector<std::string> global_names;
		global_names.reserve(global_classes_.size());
		for (const auto &[name, id] : global_classes_) {
			(void)id;
			global_names.push_back(name);
		}
		std::sort(global_names.begin(), global_names.end());
		for (const auto &name : global_names) {
			if (names.insert(name).second) {
				auto item = completion_item(name, "class", {}, SymbolKind::Class);
				item.symbol_id = global_classes_.at(name);
				item.origin_id = item.symbol_id;
				result.push_back(std::move(item));
			}
		}
		std::vector<std::pair<std::string, std::string>> autoload_names(autoloads_.begin(), autoloads_.end());
		std::sort(autoload_names.begin(), autoload_names.end());
		for (const auto &[name, path] : autoload_names) {
			if (names.insert(name).second) {
				auto item = completion_item(name, "autoload " + path, {}, SymbolKind::Variable);
				item.symbol_id = "autoload:" + name;
				result.push_back(std::move(item));
			}
		}
		for (const auto &function : gdscript_builtin_functions()) if (names.insert(std::string(function.name)).second) {
			auto item = completion_item(std::string(function.name), "func", {}, SymbolKind::Method,
				!function.signature.arguments.empty() || function.signature.is_vararg, "builtin:" + std::string(function.name));
			result.push_back(std::move(item));
		}
	}
	for (size_t index = 0; index < result.size(); ++index) {
		auto rank = std::to_string(index);
		result[index].sort_text = std::string(10 - std::min<size_t>(rank.size(), 10), '0') + rank;
	}
	return result;
}

CompletionResult Workspace::completion_result(const std::string &uri, Position position, CompletionProfile profile) const {
	std::shared_lock lock(mutex_);
	CompletionResult output;
	auto *document = find_document(uri);
	if (!document) {
		output.is_incomplete = true;
		return output;
	}
	auto site = analyze_caret(*document, position);
	if (site.role == CaretRole::Suppressed) {
		// An empty replacement is intentional: standalone clients receive no
		// candidates, and the helpers-first editor bridge consumes the request
		// instead of allowing another provider to repopulate a structural colon.
		output.disposition = CompletionDisposition::Replace;
		output.provider = "context";
		return output;
	}
	auto *context = document->class_at(position);
	auto in_type_hint = site.role == CaretRole::TypeHint;

	auto infer = [&](std::string expression) {
		std::vector<std::string> stack;
		return infer_expression(std::move(expression), *document, context, position, stack);
	};
	auto symbol_by_id = [&](std::string_view id) -> const Symbol * {
		auto found = symbols_.find(std::string(id));
		return found == symbols_.end() ? nullptr : found->second;
	};
	auto &call = site.call;
	auto callable_argument = [&](const CaretCallContext &active) -> std::optional<ExpectedValue> {
		auto callable = infer(active.callee);
		std::string declared;
		auto expected_from_declaration = [&](ResolvedType type, const ClassRecord *declaration_context,
				std::string access) -> std::optional<ExpectedValue> {
			if (!type.known()) return std::nullopt;
			AccessProvenance provenance{access,
				declaration_context ? declaration_context->symbol.id : std::string{}, {}};
			if (auto member = trailing_member(active.callee)) {
				auto receiver_type = infer(member->first);
				provenance.receiver_access = expression_type_access(member->first, receiver_type,
					*document, context, position);
			}
			return ExpectedValue{std::move(type), std::move(access), std::move(provenance)};
		};
		if (callable.symbol_id.ends_with("::new")) {
			auto owner_id = callable.symbol_id.substr(0, callable.symbol_id.size() - 5);
			if (owner_id.starts_with("native:")) {
				if (auto *constructors = native_api_.constructors(owner_id.substr(7))) {
					for (const auto &signature : *constructors) {
						if (active.argument_index < signature.arguments.size()) {
							declared = signature.arguments[active.argument_index].type;
							break;
						}
					}
				}
			} else if (auto *record = find_class(owner_id)) {
				if (auto *constructor = find_member(*record, "_init")) {
					size_t argument = 0;
					for (const auto &child : constructor->children) {
						if (!child.is_parameter) continue;
						if (argument++ != active.argument_index) continue;
						declared = child.declared_type;
						break;
					}
					if (!declared.empty()) {
						auto type = type_from_name(declared, record);
						return expected_from_declaration(std::move(type), record, declared);
					}
				}
			}
		} else if (callable.symbol_id.starts_with("native:")) {
			auto separator = callable.symbol_id.rfind("::");
			if (separator != std::string::npos) {
				auto owner = callable.symbol_id.substr(7, separator - 7);
				auto name = callable.symbol_id.substr(separator + 2);
				if (auto *member = native_api_.find_member(owner, name); member && member->signature &&
						active.argument_index < member->signature->arguments.size()) {
					declared = member->signature->arguments[active.argument_index].type;
				}
			}
		} else if (callable.symbol_id.starts_with("utility:")) {
			if (auto *signature = native_api_.find_utility_function(callable.symbol_id.substr(8)); signature &&
					active.argument_index < signature->arguments.size()) declared = signature->arguments[active.argument_index].type;
		} else if (callable.symbol_id.starts_with("builtin:")) {
			if (auto *function = find_gdscript_builtin_function(callable.symbol_id.substr(8)); function &&
					active.argument_index < function->signature.arguments.size()) declared = function->signature.arguments[active.argument_index].type;
		} else if (auto *symbol = symbol_by_id(callable.symbol_id)) {
			size_t argument = 0;
			for (const auto &child : symbol->children) {
				if (!child.is_parameter) continue;
				if (argument++ == active.argument_index) {
					declared = child.declared_type;
					break;
				}
			}
			if (!declared.empty()) {
				auto owner = symbol_owners_.find(symbol->id);
				auto *declaration_context = owner == symbol_owners_.end() ? context : find_class(owner->second);
				auto type = type_from_name(declared, declaration_context);
				return expected_from_declaration(std::move(type), declaration_context, declared);
			}
		}
		if (declared.empty()) return std::nullopt;
		auto type = type_from_name(declared, context);
		return expected_from_declaration(std::move(type), context, normalize_api_type(declared));
	};
	auto assignment_expected = [&]() -> std::optional<ExpectedValue> {
		if (site.assignment_left.empty()) return std::nullopt;
		auto lhs = site.assignment_left;
		if (lhs.starts_with("var ") || lhs.starts_with("const ")) {
			if (!site.declared_type.empty()) {
				auto declared = site.declared_type;
				auto type = type_from_name(declared, context);
				if (type.known()) return ExpectedValue{type, declared,
					AccessProvenance{declared, context ? context->symbol.id : std::string{}, {}}};
			}
		}
		auto type = infer(lhs);
		return type.known() ? std::optional<ExpectedValue>(ExpectedValue{type, type.name,
			access_provenance(lhs, type, *document, context, position)}) : std::nullopt;
	};
	auto comparison_expected = [&]() -> std::optional<ExpectedValue> {
		if (!site.operation) return std::nullopt;
		auto &operation = site.operation->operation;
		if (operation != "==" && operation != "!=" && operation != "<" && operation != "<=" &&
				operation != ">" && operation != ">=") return std::nullopt;
		auto type = infer(site.operation->left_expression);
		return type.known() ? std::optional<ExpectedValue>(ExpectedValue{type, type.name,
			access_provenance(site.operation->left_expression, type, *document, context, position)}) : std::nullopt;
	};
	auto match_expected = [&]() -> std::optional<ExpectedValue> {
		if (site.match_expression.empty()) return std::nullopt;
		auto type = infer(site.match_expression);
		return type.known() ? std::optional<ExpectedValue>(ExpectedValue{type, type.name,
			access_provenance(site.match_expression, type, *document, context, position)}) : std::nullopt;
	};
	std::optional<ExpectedValue> expected;
	switch (site.role) {
		case CaretRole::AssignmentValue:
			expected = assignment_expected();
			break;
		case CaretRole::ComparisonRight:
			expected = comparison_expected();
			break;
		case CaretRole::CallArgument:
			if (call) expected = callable_argument(*call);
			break;
		case CaretRole::MatchPattern:
			expected = match_expected();
			break;
		default:
			break;
	}
	if (!expected && site.conditional &&
			(site.role == CaretRole::ConditionalTrue || site.role == CaretRole::ConditionalFalse)) {
		auto sibling = site.conditional->branch == ConditionalBranch::TrueValue ?
			site.conditional->false_expression : site.conditional->true_expression;
		if (!sibling.empty()) {
			auto sibling_type = infer(sibling);
			auto current_expression = site.conditional->branch == ConditionalBranch::TrueValue ?
				site.conditional->true_expression : site.conditional->false_expression;
			auto current_type = current_expression.empty() ? ResolvedType::unknown() : infer(current_expression);
			auto aligned = !current_type.known() || current_type.kind == TypeKind::Variant ||
				(current_type.kind == sibling_type.kind && current_type.name == sibling_type.name &&
				 (current_type.symbol_id.empty() || sibling_type.symbol_id.empty() ||
				  current_type.symbol_id == sibling_type.symbol_id));
			if (aligned && sibling_type.known() && sibling_type.kind != TypeKind::Variant) {
				expected = ExpectedValue{sibling_type, sibling_type.name,
					access_provenance(sibling, sibling_type, *document, context, position)};
			}
		}
	}

	auto rank = [](std::vector<CompletionItem> &items, std::string_view provider) {
		for (size_t index = 0; index < items.size(); ++index) {
			auto value = std::to_string(index);
			items[index].sort_text = std::string(10 - std::min<size_t>(value.size(), 10), '0') + value;
			if (items[index].provider.empty()) items[index].provider = provider;
		}
	};
	auto merge_front = [](std::vector<CompletionItem> additions, std::vector<CompletionItem> baseline) {
		std::vector<CompletionItem> result;
		std::set<std::tuple<std::string, std::string, SymbolKind>> seen;
		auto append = [&](std::vector<CompletionItem> &items) {
			for (auto &item : items) {
				auto key = std::tuple{item.filter_text, item.insert_text, item.kind};
				if (seen.insert(key).second) result.push_back(std::move(item));
			}
		};
		append(additions);
		append(baseline);
		return result;
	};

	// String-valued method and property names are true context owners: a normal
	// identifier list is actively misleading inside these arguments.
	if (completion_config_.member_strings && call) {
		struct StringCallSpec { size_t argument; bool methods; bool first_argument_receiver; bool subpath; bool node_path; };
		static const std::unordered_map<std::string, StringCallSpec> specs = {
			{"call", {0, true, false, false, false}}, {"call_deferred", {0, true, false, false, false}},
			{"callv", {0, true, false, false, false}}, {"call_thread_safe", {0, true, false, false, false}},
			{"call_deferred_thread_group", {0, true, false, false, false}}, {"has_method", {0, true, false, false, false}},
			{"rpc", {0, true, false, false, false}}, {"rpc_id", {1, true, false, false, false}},
			{"Callable", {1, true, true, false, false}}, {"set", {0, false, false, false, false}},
			{"get", {0, false, false, false, false}}, {"set_deferred", {0, false, false, false, false}},
			{"set_indexed", {0, false, false, true, true}}, {"get_indexed", {0, false, false, true, true}},
			{"tween_property", {1, false, true, true, false}},
		};
		auto member = trailing_member(call->callee);
		auto function_name = member ? member->second : call->callee;
		if (auto spec_it = specs.find(function_name); spec_it != specs.end() && call->argument_index == spec_it->second.argument) {
			auto spec = spec_it->second;
			std::string receiver_expression;
			if (spec.first_argument_receiver) {
				if (!call->arguments.empty()) receiver_expression = call->arguments.front();
			} else if (member) receiver_expression = member->first;
			else receiver_expression = "self";
			auto receiver = infer(receiver_expression);
			if (!(receiver.kind == TypeKind::Builtin && receiver.name == "Dictionary") && receiver.known()) {
				auto current_argument = call->arguments.empty() ? std::string{} : call->arguments.back();
				std::string prefix;
				if (call->in_string) {
					auto quote_at = current_argument.rfind(call->quote);
					prefix = quote_at == std::string::npos ? current_argument : current_argument.substr(quote_at + 1);
				}
				auto target = receiver;
				std::string completed_path;
				if (spec.subpath) {
					auto separator = prefix.rfind(':');
					if (separator != std::string::npos) {
						completed_path = prefix.substr(0, separator + 1);
						auto walked = prefix.substr(0, separator);
						size_t start = 0;
						while (start < walked.size() && target.known()) {
							auto end = walked.find(':', start);
							auto segment = walked.substr(start, end - start);
							std::vector<std::string> stack;
							target = member_value_type(target, segment, *document, position, stack);
							if (end == std::string::npos) break;
							start = end + 1;
						}
					}
				}
				std::set<std::string> member_names;
				auto append_member = [&](const Symbol &symbol) {
					if (spec.methods != callable_kind(symbol.kind)) return;
					if (!spec.methods && symbol.kind != SymbolKind::Property && symbol.kind != SymbolKind::Field &&
							symbol.kind != SymbolKind::Variable) return;
					if (!completion_config_.member_strings_include_private && symbol.name.starts_with('_')) return;
					if (!member_names.insert(symbol.name).second) return;
					auto value = completed_path + symbol.name;
					CompletionItem item;
					item.label = value;
					item.filter_text = value;
					item.insert_text = call->in_string ? value :
						(spec.node_path ? "^\"" + value + "\"" :
						 (completion_config_.member_strings_prefer_string_name ? "&\"" + value + "\"" : "\"" + value + "\""));
					item.kind = symbol.kind;
					output.items.push_back(std::move(item));
				};
				if (target.kind == TypeKind::ScriptClass) {
					if (auto *record = find_class(target.symbol_id)) {
						for (auto *entry : all_members(*record)) append_member(*entry);
						auto base = native_base(*record);
						if (!base.empty()) for (auto *entry : native_api_.members(base)) {
							Symbol symbol;
							symbol.name = entry->name;
							symbol.kind = entry->kind;
							append_member(symbol);
						}
					}
				} else {
					auto native_name = target.kind == TypeKind::Callable ? std::string("Callable") : target.name;
					for (auto *entry : native_api_.members(native_name)) {
						Symbol symbol;
						symbol.name = entry->name;
						symbol.kind = entry->kind;
						append_member(symbol);
					}
				}
				if (!output.items.empty()) {
					output.disposition = CompletionDisposition::Replace;
					output.provider = "memberStrings";
				rank(output.items, output.provider);
				return output;
				}
			}
		}
	}
	if (site.lexical != CaretLexicalContext::Code) {
		// The full standalone server owns lexical contexts and must not offer
		// ordinary identifiers inside them. The helpers-only editor bridge,
		// however, must fall through when no specialized string provider handled
		// the request so Godot and other completion plugins remain available.
		if (profile == CompletionProfile::Full) {
			output.disposition = CompletionDisposition::Replace;
			output.provider = "semantic";
		}
		return output;
	}

	if (completion_config_.enums && expected && expected->type.kind == TypeKind::Enum) {
		std::vector<std::string> values;
		std::vector<AccessPath> accesses = access_paths_for_type(expected->type, context, expected->provenance);
		if (expected->type.symbol_id.starts_with("global:")) {
			values = native_api_.global_enum_values(expected->type.symbol_id.substr(7));
			accesses = {{"", AccessPathKind::Global, true}};
		} else if (expected->type.symbol_id.starts_with("nativeenum:")) {
			auto path = expected->type.symbol_id.substr(11);
			auto separator = path.rfind('.');
			if (separator != std::string::npos) {
				values = native_api_.enum_values(path.substr(0, separator), path.substr(separator + 1));
				if (accesses.empty()) accesses = {{path.substr(0, separator), AccessPathKind::Native, true}};
			}
		} else if (auto *symbol = symbol_by_id(expected->type.symbol_id)) {
			for (const auto &child : symbol->children) values.push_back(child.name);
		}
		if (accesses.empty() && !expected->access.empty()) accesses.push_back({expected->access, AccessPathKind::Local, true});
		std::set<std::string> inserted_values;
		for (const auto &access : accesses) for (const auto &name : values) {
			auto inserted = access.text.empty() ? name : access.text + "." + name;
			if (!inserted_values.insert(inserted).second) continue;
			auto item = completion_item(inserted,
				access.preferred ? "enum" : "enum (alternate " + std::string(access_path_kind_name(access.kind)) + ")",
				{}, SymbolKind::Enum);
			if (expected->type.symbol_id.starts_with("nativeenum:")) {
				auto qualified = expected->type.symbol_id.substr(11);
				auto separator = qualified.rfind('.');
				item.symbol_id = "native:" + qualified.substr(0, separator) + "::" + name;
			} else if (expected->type.symbol_id.starts_with("global:")) {
				item.symbol_id = expected->type.symbol_id + "::" + name;
			} else {
				item.symbol_id = expected->type.symbol_id + "." + name;
			}
			item.origin_id = item.symbol_id;
			item.access_kind = std::string(access_path_kind_name(access.kind));
			output.items.push_back(std::move(item));
		}
		if (!output.items.empty()) {
			output.disposition = CompletionDisposition::Replace;
			output.provider = "enums";
			rank(output.items, output.provider);
			return output;
		}
	}

	std::vector<CompletionItem> additions;
	std::string augment_provider;
	if (completion_config_.extended_type_hints && in_type_hint) {
		auto add_type = [&](std::string name, SymbolKind kind = SymbolKind::Class) {
			additions.push_back(completion_item(std::move(name),
				kind == SymbolKind::Enum ? "enum" : "class", {}, kind));
		};
		if (site.member_receiver) {
			for (auto &item : semantic_completion_locked(*document, position, site)) {
				if (item.kind == SymbolKind::Class || item.kind == SymbolKind::Enum || item.kind == SymbolKind::Constant) {
					if (item.kind == SymbolKind::Constant) {
						std::unordered_set<std::string> stack;
					auto type = resolve_static_reference(*site.member_receiver + "." + item.filter_text, context, stack);
						if (!(type.kind == TypeKind::Enum || (!type.instance && (type.kind == TypeKind::ScriptClass ||
								type.kind == TypeKind::NativeClass || type.kind == TypeKind::Builtin)))) continue;
					}
					additions.push_back(std::move(item));
				}
			}
		} else {
			std::set<std::string> names;
			for (auto *scope = context; scope; scope = enclosing_class(*scope)) {
				for (const auto &member : scope->members) {
					if ((member.kind == SymbolKind::Class || member.kind == SymbolKind::Enum) && names.insert(member.name).second) {
						add_type(member.name, member.kind);
					} else if (member.kind == SymbolKind::Constant) {
						std::unordered_set<std::string> stack;
						auto type = resolve_static_symbol(member, stack);
						if ((type.kind == TypeKind::ScriptClass || type.kind == TypeKind::Enum) && names.insert(member.name).second) {
							add_type(member.name, type.kind == TypeKind::Enum ? SymbolKind::Enum : SymbolKind::Class);
						}
					}
				}
			}
			std::vector<std::string> globals;
			for (const auto &[name, id] : global_classes_) { (void)id; globals.push_back(name); }
			std::sort(globals.begin(), globals.end());
			for (auto &name : globals) if (names.insert(name).second) add_type(std::move(name));
			std::vector<std::string> natives;
			for (const auto &[name, record] : native_api_.classes()) { (void)record; natives.push_back(name); }
			std::sort(natives.begin(), natives.end());
			for (auto &name : natives) if (names.insert(name).second) add_type(std::move(name));
		}
		if (!additions.empty()) augment_provider = "extendedTypeHints";
	}

	if (completion_config_.constructors && expected && expected->type.instance &&
			(expected->type.kind == TypeKind::ScriptClass || expected->type.kind == TypeKind::NativeClass)) {
		bool has_arguments = false;
		if (expected->type.kind == TypeKind::ScriptClass) {
			if (auto *record = find_class(expected->type.symbol_id)) if (auto *init = find_member(*record, "_init")) {
				has_arguments = std::any_of(init->children.begin(), init->children.end(), [](const Symbol &child) { return child.is_parameter; });
			}
		} else if (auto *constructors = native_api_.constructors(expected->type.name)) {
			has_arguments = std::any_of(constructors->begin(), constructors->end(), [](const CallableSignature &signature) {
				return !signature.arguments.empty() || signature.is_vararg;
			});
		}
		auto paths = access_paths_for_type(expected->type, context, expected->provenance);
		auto access = !paths.empty() ? paths.front().text :
			(expected->access.empty() ? expected->type.name : expected->access);
		CompletionItem item;
		item.filter_text = access + ".new";
		item.label = item.filter_text + (has_arguments ? "(\xe2\x80\xa6)" : "()");
		item.insert_text = item.filter_text + (has_arguments ? "(" : "()");
		item.kind = SymbolKind::Constructor;
		item.detail = "constructor";
		item.symbol_id = expected->type.symbol_id + "::new";
		item.origin_id = item.symbol_id;
		if (expected->type.kind == TypeKind::ScriptClass) {
			if (auto *record = find_class(expected->type.symbol_id)) {
				if (auto *constructor = find_member(*record, "_init")) item.origin_id = constructor->id;
			}
		}
		item.access_kind = paths.empty() ? "local" : std::string(access_path_kind_name(paths.front().kind));
		additions.insert(additions.begin(), std::move(item));
		if (augment_provider.empty()) augment_provider = "constructors";
	}

	if (profile == CompletionProfile::Helpers) {
		output.items = std::move(additions);
		if (!output.items.empty()) {
			output.disposition = CompletionDisposition::Augment;
			output.provider = std::move(augment_provider);
			rank(output.items, output.provider);
		}
		return output;
	}
	if (in_type_hint) {
		output.items = std::move(additions);
		output.disposition = CompletionDisposition::Replace;
		output.provider = augment_provider.empty() ? "extendedTypeHints" : std::move(augment_provider);
		rank(output.items, output.provider);
		return output;
	}

	output.items = merge_front(std::move(additions), semantic_completion_locked(*document, position, site));
	if (!site.suppressed_symbol.empty()) {
		std::erase_if(output.items, [&](const CompletionItem &item) {
			return item.filter_text == site.suppressed_symbol;
		});
	}
	if (completion_config_.hide_private && site.member_access && !site.member_prefix.starts_with('_')) {
		std::erase_if(output.items, [](const CompletionItem &item) { return item.filter_text.starts_with('_'); });
	}
	output.disposition = CompletionDisposition::Replace;
	output.provider = augment_provider.empty() ? "semantic" : std::move(augment_provider);
	rank(output.items, output.provider);
	return output;
}

std::vector<CompletionItem> Workspace::completion(const std::string &uri, Position position) const {
	return completion_result(uri, position, CompletionProfile::Full).items;
}

void Workspace::set_completion_config(CompletionConfig config) {
	std::unique_lock lock(mutex_);
	completion_config_ = config;
}

CompletionConfig Workspace::completion_config() const {
	std::shared_lock lock(mutex_);
	return completion_config_;
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
		auto type = hinted_type_of_symbol(*symbol, *document, position, stack);
		auto declaration = symbol->detail.empty() ? symbol->name + ": " + type.display() : symbol->detail;
		auto inferred = symbol->declared_type.empty() && !symbol->initializer.empty() &&
			type.kind != TypeKind::Variant && type.known() ? "\n\nInferred value type: `" + type.display() + "`" : "";
		return HoverResult{"**" + declaration + "**" + inferred + "\n\nDeclared in " + symbol->id, symbol->selection_range};
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

std::vector<std::string> Workspace::affected_documents(const std::vector<std::string> &changed_uris) const {
	std::shared_lock lock(mutex_);
	std::unordered_set<std::string> affected;
	std::vector<std::string> queue;
	for (const auto &uri : changed_uris) {
		if (uri.ends_with("/project.godot") || uri.ends_with(".gd.uid")) {
			for (const auto &[document_uri, document] : documents_) {
				(void)document;
				affected.insert(document_uri);
			}
			break;
		}
		if (affected.insert(uri).second) queue.push_back(uri);
	}
	for (size_t index = 0; index < queue.size(); ++index) {
		auto found = reverse_document_dependencies_.find(queue[index]);
		if (found == reverse_document_dependencies_.end()) continue;
		for (const auto &dependent : found->second) {
			if (affected.insert(dependent).second) queue.push_back(dependent);
		}
	}
	std::vector<std::string> result(affected.begin(), affected.end());
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
	for (const auto &issue : document->syntax_errors()) add("syntax-error", issue.message, issue.range);
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
					if (auto message = invalid_type_message(symbol.declared_type, &record)) {
						add("invalid-type", *message, symbol.selection_range);
					} else {
						add("unknown-type", "Could not find type \"" + symbol.declared_type + "\" in the current scope.",
							symbol.selection_range);
					}
					return;
				}
			};
			if (!member.malformed) inspect_symbol(member);
			for (const auto &local : member.children) if (!local.malformed) inspect_symbol(local);
		}
	}
	auto semantic = SemanticAnalyzer::run(*this, *document);
	result.insert(result.end(), std::make_move_iterator(semantic.begin()), std::make_move_iterator(semantic.end()));
	std::sort(result.begin(), result.end(), [](const Diagnostic &a, const Diagnostic &b) {
		if (a.range.start != b.range.start) return a.range.start < b.range.start;
		return a.code < b.code;
	});
	return result;
}

} // namespace gdscript_lsp
