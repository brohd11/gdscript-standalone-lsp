class_name AliasNamespace

const Bridge = preload("res://alias_bridge.gd")
const LocalAlias = preload("res://alias_base.gd")
const UidAlias = preload("uid://fixturebase")

class PhysicalBase:
	func physical_member() -> void:
		pass

class LocalDerived extends LocalAlias:
	pass

class UidDerived extends UidAlias:
	pass

class QualifiedPhysicalDerived extends AliasNamespace.PhysicalBase:
	pass
