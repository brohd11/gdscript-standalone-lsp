@tool
extends "res://addons/code_completions/src/class/editor_code_completion.gd"

## Priority-zero bridge from Godot's CodeEdit popup to the shared C++ service.
## A helper result can replace or augment Godot's options; an unhandled result
## deliberately leaves the existing completion pipeline alone.

const MODE_SETTING := &"plugin/code_completion/native/mode"
const TYPE_HINT_SETTING := &"plugin/code_completion/extended_type_hints/enable"
const CONSTRUCTOR_SETTING := &"plugin/code_completion/constructor/enable"
const ENUM_SETTING := &"plugin/code_completion/enum/enable"
const HIDE_PRIVATE_SETTING := &"plugin/code_completion/member_access/hide_private_properties"
const MEMBER_STRING_SETTING := &"plugin/code_completion/member_string/enable"
const MEMBER_STRING_NAME_SETTING := &"plugin/code_completion/member_string/prefer_string_name"
const MEMBER_STRING_PRIVATE_SETTING := &"plugin/code_completion/member_string/include_private"

var mode := "helpers_first"
var enums_enabled := true
var type_hints_enabled := true
var constructors_enabled := true
var hide_private_enabled := true
var member_strings_enabled := true
var member_strings_prefer_string_name := true
var member_strings_include_private := false

var _service: Object
var _versions: Dictionary[String, int] = {}


func _get_completion_settings() -> Dictionary:
	return {"priority": 0}


func register_editor_settings(settings_helper: SettingHelperEditor):
	settings_helper.subscribe_property(self, &"mode", MODE_SETTING, "helpers_first")
	settings_helper.subscribe_property(self, &"enums_enabled", ENUM_SETTING, true)
	settings_helper.subscribe_property(self, &"type_hints_enabled", TYPE_HINT_SETTING, true)
	settings_helper.subscribe_property(self, &"constructors_enabled", CONSTRUCTOR_SETTING, true)
	settings_helper.subscribe_property(self, &"hide_private_enabled", HIDE_PRIVATE_SETTING, true)
	settings_helper.subscribe_property(self, &"member_strings_enabled", MEMBER_STRING_SETTING, true)
	settings_helper.subscribe_property(self, &"member_strings_prefer_string_name", MEMBER_STRING_NAME_SETTING, true)
	settings_helper.subscribe_property(self, &"member_strings_include_private", MEMBER_STRING_PRIVATE_SETTING, false)
	settings_helper.settings_changed.connect(_on_settings_changed)


func _singleton_ready() -> void:
	if not ClassDB.class_exists(&"GDScriptLanguageService"):
		return
	_service = ClassDB.instantiate(&"GDScriptLanguageService")
	if not is_instance_valid(_service):
		return
	_apply_configuration()
	_service.open_workspace(ProjectSettings.globalize_path("res://"), {
		"configuration": _configuration(),
	})
	var script_editor := EditorInterface.get_script_editor()
	if not script_editor.script_close.is_connected(_on_script_close):
		script_editor.script_close.connect(_on_script_close)
	var filesystem := EditorInterface.get_resource_filesystem()
	if not filesystem.resources_reload.is_connected(_on_resources_changed):
		filesystem.resources_reload.connect(_on_resources_changed)
	if not filesystem.resources_reimported.is_connected(_on_resources_changed):
		filesystem.resources_reimported.connect(_on_resources_changed)


func _on_settings_changed() -> void:
	_apply_configuration()


func _configuration() -> Dictionary:
	return {
		"completion": {
			"enums": enums_enabled,
			"extendedTypeHints": type_hints_enabled,
			"constructors": constructors_enabled,
			"hidePrivate": hide_private_enabled,
			"memberStrings": {
				"enabled": member_strings_enabled,
				"preferStringName": member_strings_prefer_string_name,
				"includePrivate": member_strings_include_private,
			},
		},
	}


func _apply_configuration() -> void:
	if is_instance_valid(_service):
		_service.set_configuration(_configuration())


func _on_code_completion_requested(script_editor: CodeEdit) -> bool:
	if mode == "disabled" or not is_instance_valid(_service) or not _service.is_ready():
		return false
	var script := get_current_script() as Script
	if not is_instance_valid(script) or script.resource_path.is_empty():
		return false
	var uri := script.resource_path
	var version := script_editor.get_version()
	if _versions.get(uri, -1) != version:
		_service.update_document(uri, script_editor.text, version)
		_versions[uri] = version

	var profile := "full" if mode == "replace" else "helpers"
	var result: Dictionary = _service.completion_ex(uri, script_editor.get_caret_line(),
		script_editor.get_caret_column(), {"profile": profile})
	var disposition: String = result.get("disposition", "not_handled")
	if disposition == "not_handled":
		return false

	var existing: Array = script_editor.get_code_completion_options() if disposition == "augment" else []
	var inserts := {}
	for item: Dictionary in result.get("items", []):
		var insert_text: String = item.get("insertText", item.get("label", ""))
		if insert_text.is_empty() or inserts.has(insert_text):
			continue
		inserts[insert_text] = true
		add_completion_option(script_editor, _native_option(item))
	for option: Dictionary in existing:
		var insert_text: String = option.get("insert_text", "")
		if inserts.has(insert_text):
			continue
		inserts[insert_text] = true
		add_completion_option(script_editor, option)
	script_editor.update_code_completion_options(true)
	return true


func _native_option(item: Dictionary) -> Dictionary:
	var kind := _code_edit_kind(int(item.get("kind", 13)))
	return get_code_complete_dict(kind, item.get("label", ""), item.get("insertText", ""),
		_icon_name(kind), null, CodeEdit.LOCATION_LOCAL)


func _code_edit_kind(symbol_kind: int) -> CodeEdit.CodeCompletionKind:
	match symbol_kind:
		5, 26:
			return CodeEdit.KIND_CLASS
		6, 9, 12:
			return CodeEdit.KIND_FUNCTION
		10:
			return CodeEdit.KIND_ENUM
		14:
			return CodeEdit.KIND_CONSTANT
		24:
			return CodeEdit.KIND_SIGNAL
		7, 8:
			return CodeEdit.KIND_MEMBER
		_:
			return CodeEdit.KIND_VARIABLE


func _icon_name(kind: CodeEdit.CodeCompletionKind) -> String:
	match kind:
		CodeEdit.KIND_CLASS:
			return "Object"
		CodeEdit.KIND_FUNCTION:
			return "method"
		CodeEdit.KIND_SIGNAL:
			return "signal"
		CodeEdit.KIND_MEMBER:
			return "property"
		CodeEdit.KIND_ENUM:
			return "enum"
		CodeEdit.KIND_CONSTANT:
			return "const"
	return ""


func _on_script_close(script: Script) -> void:
	if not is_instance_valid(_service) or not is_instance_valid(script) or script.resource_path.is_empty():
		return
	_service.close_document(script.resource_path)
	_versions.erase(script.resource_path)


func _on_resources_changed(paths: PackedStringArray) -> void:
	if is_instance_valid(_service) and _service.is_ready():
		_service.refresh_files(paths)


func clean_up() -> void:
	var script_editor := EditorInterface.get_script_editor()
	if is_instance_valid(script_editor) and script_editor.script_close.is_connected(_on_script_close):
		script_editor.script_close.disconnect(_on_script_close)
	var filesystem := EditorInterface.get_resource_filesystem()
	if is_instance_valid(filesystem) and filesystem.resources_reload.is_connected(_on_resources_changed):
		filesystem.resources_reload.disconnect(_on_resources_changed)
	if is_instance_valid(filesystem) and filesystem.resources_reimported.is_connected(_on_resources_changed):
		filesystem.resources_reimported.disconnect(_on_resources_changed)
	_service = null
	_versions.clear()
	super.clean_up()
