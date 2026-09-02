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
	for item: Dictionary in completion.items:
		labels.append(item.filterText)
		completion_by_name[item.filterText] = item
	for expected in ["own", "count", "label", "reference_method"]:
		if expected not in labels:
			push_error("missing completion: %s" % expected)
			quit(1)
			return
	if completion_by_name.label.label != "label()" or completion_by_name.label.insertText != "label()":
		push_error("unexpected callable completion: %s" % [completion_by_name.label])
		quit(1)
		return
	var resolved: Dictionary = service.resolve_type(uri, 6, 4, "local")
	if resolved.kind != "script_class" or resolved.name != "ChildThing":
		push_error("unexpected resolved type: %s" % resolved)
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
