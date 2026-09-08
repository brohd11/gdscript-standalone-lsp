extends RefCounted

func strict_bool_to_int() -> int:
	return false

func strict_int_to_bool() -> bool:
	return 1

func inspect() -> void:
	var callable: Callable = func() -> void: pass
	var parts: PackedStringArray = "res://folder/file".trim_prefix("res://").split("/")
	var ordinary: Array = parts
	var packed: PackedStringArray = ordinary
	var mode: FileAccess.ModeFlags = FileAccess.WRITE
	var hash_type: HashingContext.HashType = HashingContext.HASH_SHA256
	var error: Error = ERR_ALREADY_EXISTS
	var another := new()
	callable.call()
	print(packed, mode, hash_type, error, another)
