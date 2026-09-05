class_name CompletionProviderBase
extends RefCounted

var inherited_property: String
var _inherited_private: int

func inherited_method() -> void:
	pass

func _inherited_method() -> void:
	pass

func inherited_typed(value: int = 4, ...rest) -> String:
	return str(value, rest)

static func inherited_static(label: String = "ok") -> int:
	return label.length()
