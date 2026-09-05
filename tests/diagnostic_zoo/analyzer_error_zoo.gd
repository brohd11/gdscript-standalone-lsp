# Semantically-invalid but mostly parser-valid analyzer/type-checking fixture.
# Exact wording can change across Godot revisions; ranges/recovery are the point.
extends Node


func expects_int(value: int) -> void:
    print(value)


func returns_int() -> int:
    return 1


func returns_void() -> void:
    pass


func wrong_return_type() -> int:
    return "not an int" # E: return type mismatch


func missing_return_on_path(flag: bool) -> int:
    if flag:
        return 1
    # E: not all code paths return a value


func analyzer_errors() -> void:
    print(never_declared_anywhere) # E: identifier not declared

    # Emoji before an invalid identifier stresses UTF-16 diagnostic columns.
    print("🧪", another_missing_identifier) # E: identifier not declared

    var bad_int: int = "text" # E: String cannot initialize int
    var bad_node: Node2D = Node.new() # Valid statically: implicit downcast; the runtime value may fail the check.
    print(bad_int, bad_node)

    expects_int("text") # E: argument type mismatch
    returns_int(123) # E: too many arguments

    var vector := Vector2.ZERO
    print(vector.not_a_member) # E: missing member on a hard builtin type
    vector.not_a_method() # E: missing method

    var bad_constructor := Vector2(1, 2, 3) # E: no matching constructor
    print(bad_constructor)

    var bad_operator := Vector2.ZERO + Node.new() # E: invalid operand types
    print(bad_operator)

    var impossible_cast := 12 as Node # E: statically impossible cast
    print(impossible_cast)

    var void_value = returns_void() # E: cannot use a void return value
    print(void_value)

    var typed_array: Array[int] = [1, 2, "three"] # E: typed element mismatch
    print(typed_array)
