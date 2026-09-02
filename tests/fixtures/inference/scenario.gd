extends RefCounted

signal local_signal(some_arg: int)
var typed_dict: Dictionary[String, int] = {}

func infer_cases() -> void:
	var explicit_int: int = InferenceSupport.INT_2
	for dict_key in typed_dict:
		pass
	for dict_value in typed_dict.values():
		pass
	var color_val = Color.ALICE_BLUE
	var chan_by_str = color_val["r"]
	var chan_by_idx = color_val[0]
	var html_str = color_val.to_html()
	var first_char = html_str[0]
	var lambda_ref = func(): return
	var awaited_signal_arg = await local_signal
	var signal_ref = local_signal
	var awaited_ref = await signal_ref
	var builtin_callable = char
	var builtin_ret = builtin_callable.call()
	var callable_chain_bool = some_func.bind().bind().is_null()
	var returned_callable = get_call()
	var called_signal = returned_callable.call()
	var awaited_return = await called_signal
	var returned_callable2 = get_call2()
	var called_signal2 = returned_callable2.call()
	var awaited_bool = await called_signal2
	var sig_connections = called_signal2.get_connections()
	var typed_conns: Array[String] = sig_connections
	var made_obj = InferenceSupport.new()
	var obj_string = made_obj.get_string()
	var static_string = InferenceSupport.static_get_string()
	var subscript_new = InferenceSupport["new"].call()
	var subscript_string = subscript_new.get_string()
	var made_signal = made_obj.get_signal()
	var awaited_made = await made_signal
	var line: LineEdit
	var got_variant = line.get("text")
	var menu_callable = line.get_menu
	var menu = menu_callable.call()
	var awaited_text = await line.text_changed
	var typed_map: Dictionary[InferenceEnum, InferenceEnum.Nested] = {}
	for map_key in typed_map:
		var map_val = typed_map.get(map_key)
		pass
	var packed = PackedByteArray()
	for byte in packed:
		pass
	var preload_const = preload("res://inference_enum.gd").MY_COLOR
	var cast_obj = Object.new() as InferenceEnum
	var nested_callable = InferenceSupport.Nested.node_test
	var nested_call_ret = nested_callable.call()
	var nested_direct = InferenceSupport.Nested.node_test(Node.PROCESS_MODE_ALWAYS)
	var node_ins = Node.new()
	var node_static = Node
	var pre_shadow_callable = shadowed_func
	var shadowed_func = ""
	var post_shadow = shadowed_func

func some_func() -> void:
	pass

func funk_test():
	var code: LineEdit
	return code.text_changed

func get_call():
	return funk_test

func get_call2():
	return another_sig

func another_sig():
	var support = InferenceSupport.new()
	return support.sig_bool

func terminal_before_func():
	var seed_local := Color.AQUA
	var direct_terminal = seed_local
func _after_terminal() -> void:
	pass

func shadowed_func() -> void:
	pass
