# GDScript diagnostic zoo for Godot 4.x / current master.
# Intentionally noisy. Keep this beside tool_base.gd in res://.
#
# The project.godot in this fixture enables warning categories that Godot
# normally ignores. This file targets every currently-producible warning code
# that can occur in a non-empty script; empty.gd covers EMPTY_FILE separately.

@static_unload # W: REDUNDANT_STATIC_UNLOAD
extends "res://tool_base.gd" # W: MISSING_TOOL (base is @tool, this script is not)

signal unused_signal(untyped_signal_parameter) # W: UNUSED_SIGNAL, UNTYPED_DECLARATION

var _unused_private_member: int = 1 # W: UNUSED_PRIVATE_CLASS_VARIABLE
var shadow_target: int = 10
var member_seen_before_local: int = 20

# W: GET_NODE_DEFAULT_WITHOUT_ONREADY
var node_lookup_too_early = $Child

# W: ONREADY_WITH_EXPORT
@onready @export var exported_onready_conflict: String = "conflict"

enum NoZero {
    ONE = 1,
    TWO = 2,
}

# W: ENUM_VARIABLE_WITHOUT_DEFAULT
var enum_without_zero_default: NoZero


class StaticProbe:
    static func answer() -> int:
        return 42


# Based on the engine's native-property temporary-modification warning shape.
class TemporaryModificationProbe extends Line2D:
    func trigger() -> void:
        points = PackedVector2Array([Vector2.ZERO])
        points[0] = Vector2.ONE # W: CONFUSABLE_TEMPORARY_MODIFICATION
        points[0].x = 3.0 # W: CONFUSABLE_TEMPORARY_MODIFICATION
        points.clear() # W: CONFUSABLE_TEMPORARY_MODIFICATION


# W: NATIVE_METHOD_OVERRIDE (Object.get is native)
func get(_property: StringName) -> Variant:
    return null


# W: UNTYPED_DECLARATION (no explicit return type)
func loosely_typed_return():
    return 7


func returns_variant() -> Variant:
    return Node2D.new()


func returns_int() -> int:
    return 7


func expects_node2d(value: Node2D) -> void:
    print(value)


func coroutine_probe() -> void:
    # W: REDUNDANT_AWAIT, and this also makes the function a coroutine.
    await 0


func warning_zoo(unused_parameter, used_parameter: int) -> void: # W: UNUSED_PARAMETER, UNTYPED_DECLARATION
    var unassigned
    print(unassigned) # W: UNASSIGNED_VARIABLE

    var op_unassigned
    op_unassigned += 1 # W: UNASSIGNED_VARIABLE_OP_ASSIGN

    var unused_local = 123 # W: UNUSED_VARIABLE, UNTYPED_DECLARATION
    const UNUSED_LOCAL_CONST = 456 # W: UNUSED_LOCAL_CONSTANT, INFERRED_DECLARATION

    var shadow_target = 999 # W: SHADOWED_VARIABLE, UNTYPED_DECLARATION
    var position = Vector2.ZERO # W: SHADOWED_VARIABLE_BASE_CLASS, INFERRED_DECLARATION
    var abs = 5 # W: SHADOWED_GLOBAL_IDENTIFIER, UNTYPED_DECLARATION

    var inferred_integer := 123 # W: INFERRED_DECLARATION
    print(inferred_integer)

    var untyped_copy = used_parameter # W: UNTYPED_DECLARATION
    print(untyped_copy)

    var maybe_object: Object = Node2D.new()
    # Emoji before the diagnostic target deliberately stresses UTF-16 LSP columns.
    print("💥", maybe_object.position) # W: UNSAFE_PROPERTY_ACCESS
    maybe_object.get_child(0) # W: UNSAFE_METHOD_ACCESS

    var hard_variant: Variant = Node2D.new()
    var cast_from_variant := hard_variant as Node2D # W: UNSAFE_CAST, INFERRED_DECLARATION
    print(cast_from_variant)

    # W: INFERENCE_ON_VARIANT (explicit Variant return + :=)
    var inferred_from_variant := returns_variant()
    print(inferred_from_variant)

    var base_node: Node = Node2D.new()
    expects_node2d(base_node) # W: UNSAFE_CALL_ARGUMENT

    returns_int() # W: RETURN_VALUE_DISCARDED

    var static_instance := StaticProbe.new() # W: INFERRED_DECLARATION
    static_instance.answer() # W: STATIC_CALLED_ON_INSTANCE, RETURN_VALUE_DISCARDED

    await returns_int() # W: REDUNDANT_AWAIT
    coroutine_probe() # W: MISSING_AWAIT

    assert(1 == 1) # W: ASSERT_ALWAYS_TRUE
    assert(1 == 2) # W: ASSERT_ALWAYS_FALSE

    var integer_division: int = 7 / 2 # W: INTEGER_DIVISION
    var narrowed_integer: int = 1.5 # W: NARROWING_CONVERSION
    print(integer_division, narrowed_integer)

    var enum_matching: NoZero = 1 # W: INT_AS_ENUM_WITHOUT_CAST
    var enum_not_matching: NoZero = 99 # W: INT_AS_ENUM_WITHOUT_CAST, INT_AS_ENUM_WITHOUT_MATCH
    print(enum_matching, enum_not_matching)

    1 + 2 # W: STANDALONE_EXPRESSION
    1 if used_parameter > 0 else 2 # W: STANDALONE_TERNARY

    # W: INCOMPATIBLE_TERNARY (and likely inference-related warnings by config)
    var incompatible_ternary := Node.new() if used_parameter > 0 else 123
    print(incompatible_ternary)

    match used_parameter:
        _:
            pass
        1: # W: UNREACHABLE_PATTERN
            pass


func unreachable_code_probe() -> void:
    return
    print("never reached") # W: UNREACHABLE_CODE


func confusable_local_declaration_probe() -> void:
    if true:
        var repeated_name = 1
        print(repeated_name)
    var repeated_name = 2 # W: CONFUSABLE_LOCAL_DECLARATION
    print(repeated_name)


func confusable_local_usage_probe() -> void:
    print(member_seen_before_local) # W: CONFUSABLE_LOCAL_USAGE
    var member_seen_before_local = 2 # also W: SHADOWED_VARIABLE
    print(member_seen_before_local)


func confusable_capture_probe() -> void:
    var captured := 1
    var callback := func() -> void:
        captured = 2 # W: CONFUSABLE_CAPTURE_REASSIGNMENT
    callback.call()
    print(captured)


func confusable_identifier_probe() -> void:
    # The final 'e' in this identifier is Cyrillic U+0435, not Latin U+0065.
    var usеr = 1 # W: CONFUSABLE_IDENTIFIER (requires Unicode security support)
    print(usеr)


func unsafe_void_return_probe() -> void:
    # The callee's return type is not hard-typed.
    return loosely_typed_return() # W: UNSAFE_VOID_RETURN
