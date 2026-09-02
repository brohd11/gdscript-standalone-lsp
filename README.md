# Standalone GDScript language service

This repository contains an experimental Godot 4.6 GDScript semantic core and LSP server. It indexes a project directly from disk; the Godot editor does not need to be running.

```sh
make deps
make
build/gdscript-lsp
```

The server communicates over standard input/output using LSP 3.17. An editor should launch it without project arguments; the server selects and indexes the Godot project from `workspaceFolders` or `rootUri` during the standard `initialize` request. `--project /path/to/project` remains available for fixed-root integrations. One server process serves one Godot project.

It implements incremental document synchronization, completion, hover, definition, document symbols, push and pull diagnostics, and the custom `gdscript/resolveType` request.

Native Godot APIs are read from, in priority order:

1. `--api /path/to/extension_api.json`
2. `GDSCRIPT_LSP_API=/path/to/extension_api.json`
3. the generated API used by the existing parser in the target project
4. the bundled, reduced Godot 4.6 API snapshot

A project-specific snapshot can include APIs contributed by its GDExtensions, so it takes precedence over the baseline. `make install DESTDIR=... PREFIX=...` installs both the executable and its bundled API.

Run `make test` for core and JSON-RPC integration tests. The implementation recognizes `class_name`, path- and UID-based `extends`, qualified script aliases, script resources versus scenes, inner classes, autoloads, typed containers and `:=` inference, inherited script/native members, and unsaved overlays. Ordinary `var value = expression` declarations remain dynamically typed, although their initializer is retained as a completion hint. Unsupported or ambiguous expressions degrade to `Variant`/unknown instead of consulting a running editor.

Diagnostics cover tree-sitter syntax failures, duplicate members and global classes, unresolved or cyclic inheritance, unknown explicit types, incompatible typed initializers, source-ordered name resolution, statically known members and calls, argument arity/types, and typed return paths. Unsafe property, method, and call-argument access follows the corresponding `debug/gdscript/warnings/unsafe_*` values from `project.godot`, including Godot's disabled defaults and warn/error levels. Diagnostics are recalculated across the settled project graph after overlays or watched files change. Dynamic `Variant`, `Dictionary`, node-path, and otherwise unresolved expressions deliberately remain unchecked to avoid speculative errors.

The pinned tree-sitter-gdscript dependency receives a tracked compatibility patch during `make deps`. The patch adds Godot-valid inline `if` lambda bodies and fixes indentation across comment-only lines; dependency setup verifies the exact upstream commit before applying it.

To compare stable rule categories with the real Godot 4.6 parser, run:

```sh
make test-conformance GODOT=/path/to/godot
```

The oracle cases and normalized expectations live in `tests/fixtures/diagnostics/oracle` and `tests/diagnostic_oracle.json`. See `THIRD_PARTY_NOTICES.md` for Godot attribution.

## GDExtension

Fetch the pinned Godot 4.6 bindings and SCons, build the adapter, and run its headless smoke test with:

```sh
make gdextension
make test-gdextension
```

Set `GODOT=/path/to/godot` if the executable is not on `PATH`. `GDScriptLanguageService` indexes asynchronously and exposes LSP-shaped `completion`, `hover`, `definition`, `document_symbols`, `diagnostics`, and `resolve_type` methods to GDScript. `update_document`, `close_document`, and `refresh_files` keep the shared index current without `GDScript` resources or editor services.

The adapter emits `workspace_ready`, `workspace_error`, `index_updated`, and `diagnostics_updated`; callers should wait for readiness before querying. Copy `addons/gdscript_lsp` into a project to package the extension, native metadata, and platform libraries together.

## Architecture

- `src/core`: engine-neutral C++20 parser, symbol graph, inheritance resolver, type inference, and query API.
- `src/lsp`: LSP 3.17 JSON-RPC transport over stdin/stdout.
- `src/gdextension`: thin godot-cpp wrapper over the same `Workspace` API.
- `addons/gdscript_lsp/data`: reduced Godot 4.6 native class baseline.

The index is deliberately multi-pass: all scripts are parsed first, global/path/UID class identities are registered next, statically resolvable script constants and qualified aliases are linked, then base edges are settled and cycle-checked. Queries walk that completed graph, which removes the old parser's dependency on editor-created `GDScript` resources and file load order.
