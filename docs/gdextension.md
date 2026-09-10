# GDExtension integration

The GDExtension exposes the same language-service core to GDScript. Fetch the
pinned Godot 4.6 bindings and SCons, build the adapter, and run its headless
smoke test with:

```sh
make gdextension
make test-gdextension
```

Set `GODOT=/path/to/godot` if the executable is not on `PATH`.

`GDScriptLanguageService` indexes asynchronously and exposes `completion`,
`hover`, `definition`, `document_symbols`, `diagnostics`, `resolve_type`, and
`resolve_expression`. Use `update_document`, `close_document`, and
`refresh_files` to keep its index current without loading `GDScript` resources
or editor services.

`completion_ex(uri, line, column, {"profile": "helpers"})` also returns a
`disposition` of `not_handled`, `augment`, or `replace`, plus the responsible
provider. The default `full` profile returns a complete standalone completion
list. `set_configuration()` accepts the `gdscriptLsp` object described in
[configuration](configuration.md).

The adapter emits `workspace_ready`, `workspace_error`, `index_updated`, and
`diagnostics_updated`. After an update, `diagnostics_updated` contains the
changed document and its transitive dependents. Callers should wait for
`workspace_ready` before querying.

Copy `addons/gdscript_lsp` into a project to package the extension, native
metadata, and platform libraries together.

## Godot editor completion bridge

When the Code Completions addon is installed, enable the **Standalone GDScript
Language Service** editor plugin. It registers a priority-zero provider and
defaults to `plugin/code_completion/native/mode = "helpers_first"`. Native
helpers replace or augment the contexts they own; other requests continue to
Godot and the existing GDScript providers.

Set the mode to `"replace"` to use the native service for the complete popup, or
`"disabled"` to bypass it. The bridge falls through while its asynchronous
index is not ready. It synchronizes an edited buffer on Godot's debounced
completion request when the `CodeEdit` version changes.

Godot's internal code completion must remain enabled. Its network LSP server
does not need to be disabled.
