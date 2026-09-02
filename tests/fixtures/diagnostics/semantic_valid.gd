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
	callable.call(2)
	for item in [1, 2]:
		print(item)
	match data:
		{"value": var matched}:
			print(matched)
	if flag:
		return 1
	else:
		return 2
