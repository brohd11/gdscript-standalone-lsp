class_name ProvenanceBase
extends RefCounted

enum State { IDLE, ACTIVE }
const StateAlias = State

class Bundle:
	class Layer:
		enum Tag { ONE, TWO }

class Inner:
	enum Mode { A, B }
	func use_mode(value: Mode) -> void:
		pass

func make_state() -> State:
	return State.IDLE

func use_tag(value: Bundle.Layer.Tag) -> void:
	pass
