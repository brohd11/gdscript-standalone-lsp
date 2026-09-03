extends RefCounted

const TF = ContextRoot.Utils.Profile.TimeFunction.TimeScale

enum LocalState { IDLE, READY }
enum OtherState { FIRST, SECOND }

func consume(value: ContextRoot.Utils.Profile.TimeFunction.TimeScale) -> void:
	pass

func inspect() -> void:
	pass
