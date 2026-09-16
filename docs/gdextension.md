# Godot addon integration

The Godot GDExtension and editor integration live in
[godot-gdscript-lsp](https://github.com/brohd11/godot-gdscript-lsp).
That repository pins this core as a source submodule and publishes one addon ZIP
containing libraries for Windows/Linux x86_64 and macOS Intel/Apple Silicon.

Extract the ZIP into a project to install `addons/addon_lib/gdscript_lsp`.
There is no editor plugin to enable and no AddonLib dependency in the service.
AddonLib and SyntaxPlus use it as an optional parser backend; Code Completions
owns its optional native completion provider. Without the addon those plugins
retain their GDScript paths.

The plain Node `GDScriptLSPService` is shared through
`EditorNode/EditorSingletons/GDScriptLSPService`. `GDScriptLSPCodeEditManager`
shares buffer synchronization across consumers, including reads before
`text_changed`. Syntax is available while the workspace indexes asynchronously.

The core's `Document` exposes a borrowed, read-only concrete tree and edit
metadata. `Workspace::update_document(const Document &)` ingests a parsed
snapshot without reparsing and isolates its mutable semantic records.
Godot dictionaries and bracket maintenance belong to the consumer adapter.

See the consumer README for native APIs, builds, tests, and release packaging.
