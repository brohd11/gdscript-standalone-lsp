# Parser-recovery stress fixture.
# These are intentionally syntactically/grammatically invalid in independent
# functions or declarations so a language server can test multi-error recovery.
extends Node


func break_outside_loop() -> void:
    break # E: break outside loop


func continue_outside_loop() -> void:
    continue # E: continue outside loop


func removed_yield() -> void:
    yield() # E: `yield` was removed in Godot 4; use `await`


func legacy_question_mark_ternary(a, b, c) -> void:
    a ? b : c # E: unexpected `?`; GDScript ternary is `b if a else c`


func standalone_lambda() -> void:
    func() -> void: # E: standalone lambda cannot be accessed
        pass


# E: nested typed collections are not supported.
var nested_typed_array: Array[Array[int]] = []

# E: `void` is only legal as a function return type.
var void_typed_variable: void


@definitely_not_a_real_annotation
func unknown_annotation() -> void:
    pass


func required_after_default(first = 1, second) -> void:
    print(first, second) # E: required parameter after an optional/default one
