class_name CompletionBase
extends RefCounted

class SettingHelperEditor extends RefCounted:
	func subscribe_property(_target: Object, _property: StringName) -> void:
		pass

static func register_plugin(_plugin: EditorPlugin) -> void:
	pass

static func unregister_plugin(_plugin: EditorPlugin) -> void:
	pass

func get_current_script():
	return null

func add_completion_option(_editor: CodeEdit, _option: Dictionary) -> void:
	pass

func get_code_complete_dict() -> Dictionary:
	return {}

func clean_up() -> void:
	pass
