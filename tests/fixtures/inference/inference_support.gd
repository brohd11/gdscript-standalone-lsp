class_name InferenceSupport
extends RefCounted

const INT_2 := 2
const TYPED_DICT: Dictionary[String, int] = {"a": 1}
const MY_COLOR := Color.RED

signal sig_bool(flag: bool)

class Nested:
	static func node_test(mode: Node.ProcessMode) -> Node.ProcessMode:
		return mode

func get_signal():
	return sig_bool

func get_string(value := "") -> String:
	return value

func get_int() -> int:
	return 1

static func static_get_string() -> String:
	return ""
