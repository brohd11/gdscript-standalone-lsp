@tool
extends EditorPlugin

const EditorCodeCompletion = preload("res://addons/code_completions/src/class/editor_code_completion.gd")
const NativeCompletion = preload("res://addons/gdscript_lsp/editor/native_completion.gd")

var _completion: NativeCompletion


func _enter_tree() -> void:
	EditorCodeCompletion.register_plugin(self)
	_completion = NativeCompletion.new()


func _exit_tree() -> void:
	if is_instance_valid(_completion):
		_completion.clean_up()
	_completion = null
	EditorCodeCompletion.unregister_plugin(self)
