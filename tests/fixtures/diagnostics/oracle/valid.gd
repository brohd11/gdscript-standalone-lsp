extends RefCounted

var value: float = 1

class Nested:
	var values = []

	func offset_popup(offset_y := -1):
	# Comment-only lines do not determine a body's indentation.
		values.clear()

	func later():
		values.clear()

func nullable_return() -> Object:
	return

func dynamic_assignment() -> void:
	var dynamic = "text"
	dynamic = 1
	print(dynamic)

func inline_lambda(flag: bool) -> void:
	var callback := func(): if flag: print("inline")
	callback.call()
