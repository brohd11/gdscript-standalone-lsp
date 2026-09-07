extends "res://completion_base.gd"

func register_editor_settings(settings_helper: SettingHelperEditor) -> void:
	settings_helper.subscribe_property(self, &"enabled")

func request_completion(script_editor: CodeEdit) -> void:
	var script := get_current_script()
	if script != null:
		add_completion_option(script_editor, get_code_complete_dict())

func clean_up() -> void:
	super.clean_up()
