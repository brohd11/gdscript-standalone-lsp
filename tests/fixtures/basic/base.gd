class_name BaseThing extends RefCounted

var count: int = 1
const BASE_CONSTANT := 2

static func base_static() -> void:
	pass

func label() -> String:
	return "base"

func shared_name() -> String:
	return "base"
