extends RefCounted

var duplicate: int = 1
var duplicate: String = "two"
var missing: DoesNotExist
var bad: int = "wrong"

func inspect(value: MissingParameter = 1) -> void:
	var local_bad: int = "wrong"
