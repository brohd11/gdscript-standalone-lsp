extends RefCounted

var child := ChildThing.new()

func inspect_child() -> void:
	var local: ChildThing = child
	local.label()
