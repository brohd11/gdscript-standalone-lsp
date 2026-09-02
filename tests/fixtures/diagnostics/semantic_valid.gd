extends RefCounted

var checked_accessor: int:
	set(value):
		print(value)
	get:
		return 1

func accepts_base(value: DiagnosticBase) -> void:
	value.base_value = 2

func variadic(first: int, ...rest) -> void:
	print(first, rest)

func inspect(child: DiagnosticChild, dynamic: Variant, data: Dictionary, flag: bool) -> int:
	accepts_base(child)
	dynamic.anything(1, 2, 3)
	data.anything
	print("valid utility")
	Engine.get_version_info()
	var native_enum := Engine.Mode.MODE_ONE
	var global_enum := TestGlobal.TEST_VALUE
	native_takes(1)
	native_takes(1, "label")
	variadic(1, 2, 3)
	var converted := String(1)
	var rectangle := Rect2()
	var captured := 1
	var callable := func(value: int) -> int:
		return captured + value
	var typed_callable: Callable = callable
	callable.call(2)
	typed_callable.call(2)
	var path_parts: PackedStringArray = "res://folder/file".trim_prefix("res://").split("/")
	var ordinary_array: Array = path_parts
	var packed_again: PackedStringArray = ordinary_array
	path_parts.append_array(ordinary_array)
	ordinary_array.append_array(path_parts)
	var mode: FileAccess.ModeFlags = FileAccess.WRITE
	var hash_type: HashingContext.HashType = HashingContext.HASH_SHA256
	var error: Error = ERR_ALREADY_EXISTS
	var another := new()
	print(mode, hash_type, error, packed_again, another)
	for item in [1, 2]:
		print(item)
	match data:
		{"value": var matched}:
			print(matched)
	if flag:
		return 1
	else:
		return 2

func strict_bool_to_int() -> int:
	return false

func strict_int_to_bool() -> bool:
	return 1
