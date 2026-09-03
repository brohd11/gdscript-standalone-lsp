extends SceneTree

var service: Object
var diagnostics_signal_received := false

func _initialize() -> void:
	var load_status := GDExtensionManager.load_extension("res://addons/gdscript_lsp/gdscript_lsp.gdextension")
	if load_status not in [GDExtensionManager.LOAD_STATUS_OK, GDExtensionManager.LOAD_STATUS_ALREADY_LOADED]:
		push_error("could not load GDExtension: %s" % load_status)
		quit(1)
		return
	service = ClassDB.instantiate("GDScriptLanguageService")
	if service == null:
		push_error("GDScriptLanguageService was not registered")
		quit(1)
		return
	service.workspace_ready.connect(_on_ready)
	service.workspace_error.connect(_on_error)
	service.diagnostics_updated.connect(_on_diagnostics_updated)
	var error: Variant = service.open_workspace("res://tests/fixtures/basic", {
		"native_api_path": "res://tests/fixtures/basic/extension_api.json",
	})
	if error != OK:
		push_error("open_workspace returned %s" % error)
		quit(1)

func _on_ready() -> void:
	var uri := "file://" + ProjectSettings.globalize_path("res://tests/fixtures/basic/consumer.gd")
	var completion: Dictionary = service.completion(uri, 6, 7)
	var labels: Array[String] = []
	var completion_by_name: Dictionary = {}
	var previous_sort_text := ""
	for item: Dictionary in completion.items:
		labels.append(item.filterText)
		completion_by_name[item.filterText] = item
		if previous_sort_text != "" and item.sortText <= previous_sort_text:
			push_error("completion sortText is not increasing: %s then %s" % [previous_sort_text, item.sortText])
			quit(1)
			return
		previous_sort_text = item.sortText
	for expected in ["own", "count", "label", "reference_method"]:
		if expected not in labels:
			push_error("missing completion: %s" % expected)
			quit(1)
			return
	if completion_by_name.label.label != "label()" or completion_by_name.label.insertText != "label()":
		push_error("unexpected callable completion: %s" % [completion_by_name.label])
		quit(1)
		return
	if labels.find("own") >= labels.find("CHILD_CONSTANT") or labels.find("CHILD_CONSTANT") >= labels.find("count"):
		push_error("unexpected completion relevance order: %s" % [labels])
		quit(1)
		return
	var helper_fallback: Dictionary = service.completion_ex(uri, 6, 7, {"profile": "helpers"})
	if helper_fallback.disposition != "not_handled" or not helper_fallback.items.is_empty():
		push_error("ordinary helper completion should fall through: %s" % [helper_fallback])
		quit(1)
		return
	var resource_completion: Dictionary = service.completion("res://consumer.gd", 6, 7)
	if resource_completion.items.is_empty():
		push_error("res:// URI did not resolve against the opened workspace")
		quit(1)
		return
	var helper_source := "class_name HelperConsumer extends RefCounted\n\nfunc inspect() -> void:\n\tvar child: ChildThing = ChildThing.new()\n"
	service.update_document(uri, helper_source, 6)
	var constructor: Dictionary = service.completion_ex(uri, 3, 25, {"profile": "helpers"})
	if constructor.disposition != "augment" or constructor.provider != "constructors":
		push_error("constructor helper did not augment: %s" % [constructor])
		quit(1)
		return
	service.set_configuration({"completion": {"constructors": false}})
	constructor = service.completion_ex(uri, 3, 25, {"profile": "helpers"})
	if constructor.disposition != "not_handled":
		push_error("disabled constructor helper still handled: %s" % [constructor])
		quit(1)
		return
	service.set_configuration({"completion": {"constructors": true}})
	service.close_document(uri)
	var resolved: Dictionary = service.resolve_type(uri, 6, 4, "local")
	if resolved.kind != "script_class" or resolved.name != "ChildThing":
		push_error("unexpected resolved type: %s" % resolved)
		quit(1)
		return
	var rich: Dictionary = service.resolve_expression(uri, 6, 4, "local")
	if rich.type.name != "ChildThing" or rich.origin == null or rich.origin.name != "local":
		push_error("unexpected rich expression: %s" % [rich])
		quit(1)
		return
	if rich.accessPaths.is_empty() or not rich.accessPaths[0].preferred:
		push_error("rich expression did not expose a preferred access path: %s" % [rich])
		quit(1)
		return
	service.update_document(uri, "extends RefCounted\n\nvar wrong: int = \"text\"\n", 8)
	var diagnostics: Array = service.diagnostics(uri)
	if diagnostics.size() != 1 or diagnostics[0].code != "type-mismatch":
		push_error("unexpected diagnostics: %s" % [diagnostics])
		quit(1)
		return
	service.update_document(uri, "extends RefCounted\n\nfunc inspect() -> int:\n\tmissing_function()\n", 9)
	diagnostics = service.diagnostics(uri)
	var semantic_codes: Array[String] = []
	for diagnostic: Dictionary in diagnostics:
		semantic_codes.append(diagnostic.code)
	for expected in ["undefined-function", "missing-return-path"]:
		if expected not in semantic_codes:
			push_error("missing semantic diagnostic %s in %s" % [expected, diagnostics])
			quit(1)
			return
	if not diagnostics_signal_received:
		push_error("diagnostics_updated was not emitted")
		quit(1)
		return
	print("gdextension smoke test passed")
	quit(0)

func _on_diagnostics_updated(_paths: PackedStringArray) -> void:
	diagnostics_signal_received = true

func _on_error(message: String) -> void:
	push_error(message)
	quit(1)
