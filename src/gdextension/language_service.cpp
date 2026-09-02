#include "gdextension/language_service.hpp"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace gdscript_lsp {
namespace {

std::string to_std(const String &value) {
	CharString utf8 = value.utf8();
	return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

String to_godot(std::string_view value) {
	return String::utf8(value.data(), static_cast<int64_t>(value.size()));
}

Dictionary position_dict(Position value) {
	Dictionary result;
	result["line"] = static_cast<int64_t>(value.line);
	result["character"] = static_cast<int64_t>(value.character);
	return result;
}

Dictionary range_dict(Range value) {
	Dictionary result;
	result["start"] = position_dict(value.start);
	result["end"] = position_dict(value.end);
	return result;
}

Dictionary symbol_dict(const Symbol &symbol) {
	Dictionary result;
	result["name"] = to_godot(symbol.name);
	result["detail"] = to_godot(symbol.detail);
	result["kind"] = static_cast<int64_t>(symbol.kind);
	result["range"] = range_dict(symbol.range);
	result["selectionRange"] = range_dict(symbol.selection_range);
	Array children;
	for (const auto &child : symbol.children) children.push_back(symbol_dict(child));
	if (!children.is_empty()) result["children"] = children;
	return result;
}

Dictionary type_dict(const ResolvedType &type) {
	Dictionary result;
	result["kind"] = to_godot(type_kind_name(type.kind));
	result["name"] = to_godot(type.name);
	result["display"] = to_godot(type.display());
	result["symbolId"] = to_godot(type.symbol_id);
	result["instance"] = type.instance;
	Array arguments;
	for (const auto &argument : type.arguments) arguments.push_back(type_dict(argument));
	result["arguments"] = arguments;
	return result;
}

Dictionary diagnostic_dict(const Diagnostic &diagnostic) {
	Dictionary result;
	result["code"] = to_godot(diagnostic.code);
	result["message"] = to_godot(diagnostic.message);
	result["severity"] = static_cast<int64_t>(diagnostic.severity);
	result["source"] = to_godot(diagnostic.source);
	result["range"] = range_dict(diagnostic.range);
	Array related;
	for (const auto &item : diagnostic.related_information) {
		Dictionary value;
		value["uri"] = to_godot(item.location.uri);
		value["range"] = range_dict(item.location.range);
		value["message"] = to_godot(item.message);
		related.push_back(value);
	}
	result["relatedInformation"] = related;
	return result;
}

PackedStringArray all_document_paths(const Workspace &workspace) {
	PackedStringArray paths;
	for (const auto &uri : workspace.document_uris()) paths.push_back(to_godot(uri));
	return paths;
}

} // namespace

GDScriptLanguageService::GDScriptLanguageService() : workspace_(std::make_unique<Workspace>()) {}

GDScriptLanguageService::~GDScriptLanguageService() {
	if (index_thread_.joinable()) {
		index_thread_.request_stop();
		index_thread_.join();
	}
}

void GDScriptLanguageService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open_workspace", "project_root", "options"),
		&GDScriptLanguageService::open_workspace, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("is_ready"), &GDScriptLanguageService::is_ready);
	ClassDB::bind_method(D_METHOD("update_document", "uri", "text", "version"),
		&GDScriptLanguageService::update_document);
	ClassDB::bind_method(D_METHOD("close_document", "uri"), &GDScriptLanguageService::close_document);
	ClassDB::bind_method(D_METHOD("refresh_files", "paths"), &GDScriptLanguageService::refresh_files);
	ClassDB::bind_method(D_METHOD("completion", "uri", "line", "utf16_column"),
		&GDScriptLanguageService::completion);
	ClassDB::bind_method(D_METHOD("hover", "uri", "line", "utf16_column"),
		&GDScriptLanguageService::hover);
	ClassDB::bind_method(D_METHOD("definition", "uri", "line", "utf16_column"),
		&GDScriptLanguageService::definition);
	ClassDB::bind_method(D_METHOD("document_symbols", "uri"), &GDScriptLanguageService::document_symbols);
	ClassDB::bind_method(D_METHOD("diagnostics", "uri"), &GDScriptLanguageService::diagnostics);
	ClassDB::bind_method(D_METHOD("resolve_type", "uri", "line", "utf16_column", "expression"),
		&GDScriptLanguageService::resolve_type, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("_finish_open", "error"), &GDScriptLanguageService::_finish_open);
	ADD_SIGNAL(MethodInfo("workspace_ready"));
	ADD_SIGNAL(MethodInfo("workspace_error", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("index_updated", PropertyInfo(Variant::PACKED_STRING_ARRAY, "paths")));
	ADD_SIGNAL(MethodInfo("diagnostics_updated", PropertyInfo(Variant::PACKED_STRING_ARRAY, "paths")));
}

Error GDScriptLanguageService::open_workspace(const String &project_root, const Dictionary &options) {
	if (index_thread_.joinable()) {
		index_thread_.request_stop();
		index_thread_.join();
	}
	ready_.store(false);
	String root = project_root;
	if (root.begins_with("res://")) root = ProjectSettings::get_singleton()->globalize_path(root);
	String api;
	if (options.has("native_api_path")) api = options["native_api_path"];
	if (api.begins_with("res://")) api = ProjectSettings::get_singleton()->globalize_path(api);
	index_thread_ = std::jthread([this, root_value = to_std(root), api_value = to_std(api)] {
		std::string error;
		workspace_->open(root_value, api_value, &error);
		call_deferred("_finish_open", to_godot(error));
	});
	return OK;
}

bool GDScriptLanguageService::is_ready() const {
	return ready_.load();
}

void GDScriptLanguageService::_finish_open(const String &error) {
	ready_.store(error.is_empty());
	if (error.is_empty()) {
		emit_signal("workspace_ready");
		emit_signal("diagnostics_updated", all_document_paths(*workspace_));
	} else {
		emit_signal("workspace_error", error);
	}
}

void GDScriptLanguageService::update_document(const String &uri, const String &text, int64_t version) {
	if (!ready_) return;
	std::string error;
	workspace_->update_document(to_std(uri), to_std(text), version, &error);
	PackedStringArray changed_paths;
	changed_paths.push_back(uri);
	emit_signal("index_updated", changed_paths);
	emit_signal("diagnostics_updated", all_document_paths(*workspace_));
}

void GDScriptLanguageService::close_document(const String &uri) {
	if (!ready_) return;
	std::string error;
	workspace_->close_document(to_std(uri), &error);
	emit_signal("diagnostics_updated", all_document_paths(*workspace_));
}

void GDScriptLanguageService::refresh_files(const PackedStringArray &paths) {
	if (!ready_) return;
	for (const auto &path : paths) {
		std::string error;
		workspace_->refresh_file(to_std(path), &error);
	}
	emit_signal("index_updated", paths);
	emit_signal("diagnostics_updated", all_document_paths(*workspace_));
}

Dictionary GDScriptLanguageService::completion(const String &uri, int line, int utf16_column) const {
	Dictionary result;
	Array items;
	if (ready_) {
		for (const auto &item : workspace_->completion(to_std(uri),
					{static_cast<uint32_t>(line), static_cast<uint32_t>(utf16_column)})) {
			Dictionary value;
			value["label"] = to_godot(item.label);
			value["detail"] = to_godot(item.detail);
			value["documentation"] = to_godot(item.documentation);
			value["kind"] = static_cast<int64_t>(item.kind);
			value["insertText"] = to_godot(item.insert_text);
			items.push_back(value);
		}
	}
	result["isIncomplete"] = !ready_;
	result["items"] = items;
	return result;
}

Dictionary GDScriptLanguageService::hover(const String &uri, int line, int utf16_column) const {
	Dictionary result;
	if (!ready_) return result;
	auto value = workspace_->hover(to_std(uri), {static_cast<uint32_t>(line), static_cast<uint32_t>(utf16_column)});
	if (!value) return result;
	Dictionary contents;
	contents["kind"] = "markdown";
	contents["value"] = to_godot(value->markdown);
	result["contents"] = contents;
	result["range"] = range_dict(value->range);
	return result;
}

Array GDScriptLanguageService::definition(const String &uri, int line, int utf16_column) const {
	Array result;
	if (!ready_) return result;
	for (const auto &location : workspace_->definition(to_std(uri),
				{static_cast<uint32_t>(line), static_cast<uint32_t>(utf16_column)})) {
		Dictionary value;
		value["uri"] = to_godot(location.uri);
		value["range"] = range_dict(location.range);
		result.push_back(value);
	}
	return result;
}

Array GDScriptLanguageService::document_symbols(const String &uri) const {
	Array result;
	if (!ready_) return result;
	for (const auto &symbol : workspace_->document_symbols(to_std(uri))) result.push_back(symbol_dict(symbol));
	return result;
}

Array GDScriptLanguageService::diagnostics(const String &uri) const {
	Array result;
	if (!ready_) return result;
	for (const auto &diagnostic : workspace_->diagnostics(to_std(uri))) result.push_back(diagnostic_dict(diagnostic));
	return result;
}

Dictionary GDScriptLanguageService::resolve_type(const String &uri, int line, int utf16_column,
		const String &expression) const {
	if (!ready_) return type_dict(ResolvedType::unknown("indexing"));
	return type_dict(workspace_->resolve_type(to_std(uri),
		{static_cast<uint32_t>(line), static_cast<uint32_t>(utf16_column)}, to_std(expression)));
}

} // namespace gdscript_lsp
