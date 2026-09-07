extends RefCounted

const RESOURCE_PATH = "res://some/resource.gd"
const SCENE_RESOURCE = preload("res://scene_resource.tscn")

var typed_dictionary: Dictionary[String, Dictionary] = {}

class ItemParams:
	const POSITION = "position"
	enum Position {
		TOP,
		BOTTOM,
	}

class Scanner:
	func depth() -> int:
		return 0

func _init(
	_first,
	_second,
	_third,
	_fourth,
	_fifth,
) -> void:
	pass

func enum_probe(metadata: Dictionary) -> void:
	var position = metadata.get(ItemParams.POSITION, ItemParams.Position.TOP)
	if position == ItemParams.Position.TOP:
		pass

func builtin_probe(node: Node, expected_type: Variant) -> void:
	if is_instance_of(node, expected_type):
		pass

func initializer_hint_probe() -> void:
	var scanner = Scanner.new()
	scanner.depth()
