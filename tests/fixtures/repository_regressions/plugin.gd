extends EditorPlugin

const Completion = preload("res://completion_base.gd")
const NativeCompletion = preload("res://native_completion.gd")

var completion: NativeCompletion

func _enter_tree() -> void:
	Completion.register_plugin(self)
	completion = NativeCompletion.new()

func _exit_tree() -> void:
	if completion != null:
		completion.clean_up()
	Completion.unregister_plugin(self)
