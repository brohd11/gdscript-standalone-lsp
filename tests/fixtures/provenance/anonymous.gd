extends RefCounted

class Payload:
	var value: int

class Outer:
	class Deep:
		var value: int

func make() -> Payload:
	return Payload.new()

func make_deep() -> Outer.Deep:
	return Outer.Deep.new()
