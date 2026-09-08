extends RefCounted

const WarningBase = preload("res://base.gd")
const WarningChild = preload("res://child.gd")

func accepts_child(value: WarningChild) -> void:
	print(value)

func inspect(base: WarningBase) -> WarningChild:
	accepts_child(base)
	var child: WarningChild = base
	return base
