extends RefCounted

const Derived = preload("res://derived.gd")
const Anonymous = preload("res://anonymous.gd")

class Inner:
	enum Mode { DECOY }

func inspect() -> void:
	var object := Derived.new()
	var state := object.make_state()
	if state == ProvenanceBase.State.IDLE: pass
	object.use_tag(ProvenanceBase.Bundle.Layer.Tag.ONE)
	var inherited_inner := object.Inner.new()
	inherited_inner.use_mode(ProvenanceBase.Inner.Mode.A)
	var anonymous := Anonymous.new()
	var payload := anonymous.make()
	payload = null
	var deep := anonymous.make_deep()
	deep = null
