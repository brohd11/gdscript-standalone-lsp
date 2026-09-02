class_name ChildThing extends BaseThing

var own := "child"
const CHILD_CONSTANT := 3

static func child_static() -> void:
	pass

func make_base() -> BaseThing:
	return self

func shared_name() -> String:
	return "child"
