extends RefCounted

var broken_accessor: int:
	set(value):
		return 1
	get:
		return "wrong"

func accepts(value: int, optional: String = "ok") -> String:
	return optional

func incomplete(flag: bool) -> int:
	if flag:
		return 1

func bad_void() -> void:
	return 1

func empty_return() -> int:
	return

func inspect(base: DiagnosticBase, dynamic: Variant, data: Dictionary) -> int:
	missing_identifier
	missing_function()
	base.missing_member
	base.base_value()
	accepts()
	accepts("wrong")
	native_takes()
	native_takes("wrong")
	dynamic.anything(1, 2, 3)
	data.anything
	print("valid utility")
	Engine.get_version_info()
	if true:
		var block_value := 1
	block_value
	var bad_lambda := func(value: String) -> int:
		return value
	return "wrong"
