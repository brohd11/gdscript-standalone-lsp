# Standalone GDScript language service

This repository contains an experimental Godot 4.6 GDScript semantic core and LSP server. It indexes a project directly from disk; the Godot editor does not need to be running.

```sh
make deps
make
build/gdscript-lsp
```

The server communicates over standard input/output using LSP 3.17. An editor should launch it without project arguments; the server selects and indexes the Godot project from `workspaceFolders` or `rootUri` during the standard `initialize` request. `--project /path/to/project` remains available for fixed-root integrations. One server process serves one Godot project.

It implements incremental document synchronization, completion, completion-item resolve, hover, definition, document symbols, push and pull diagnostics, and the custom `gdscript/resolveType` and `gdscript/resolveExpression` requests.

Callable completions follow Godot's compact presentation: `name()` for zero arguments and `name(…)` otherwise. Parameterized calls insert a trailing `(` so clients with bracket pairing place the caret inside the call. Completion results are ranked by lexical scope and nearest-first inheritance, with type-level members behind instance members at each level; class receivers omit instance-only members. The array order and LSP `sortText` carry the same ranking.

Every semantic completion item carries an opaque declaration ID in `data.gdscriptLsp`, along with its provider and access-path kind. Clients may pass the item to standard `completionItem/resolve` for declaration-backed detail and documentation. `gdscript/resolveExpression` accepts the same parameters as `gdscript/resolveType`, but returns `{ type, origin, accessPaths }`: `origin` identifies the member, local, or native API declaration that produced the value, and `accessPaths` lists caller-verified spellings in preferred order (preload alias, local/inherited, then global). Clients should treat these IDs as opaque.

Portable completion helpers add expected enum values, extended type-hint names, expected-type constructors, and method/property names used as strings. Private members are hidden from ordinary member completion until the typed prefix starts with `_`. The helpers are built into the shared core, so they behave the same way over stdio and through the GDExtension.

Completion settings can be supplied as `initializationOptions.gdscriptLsp` and changed later with `workspace/didChangeConfiguration`:

```json
{
  "gdscriptLsp": {
    "completion": {
      "enums": true,
      "extendedTypeHints": true,
      "constructors": true,
      "hidePrivate": true,
      "memberStrings": {
        "enabled": true,
        "preferStringName": true,
        "includePrivate": false
      }
    },
    "diagnostics": {
      "pollIntervalMs": 1000
    }
  }
}
```

`diagnostics.pollIntervalMs` controls the portable disk-change poll used when a client does not send file-watch notifications. Positive values are clamped to at least 100 ms; `0` disables polling.

Native Godot APIs are read from, in priority order:

1. `--api /path/to/extension_api.json`
2. `GDSCRIPT_LSP_API=/path/to/extension_api.json`
3. the generated API used by the existing parser in the target project
4. the bundled, reduced Godot 4.6 API snapshot

A project-specific snapshot can include APIs contributed by its GDExtensions, so it takes precedence over the baseline. `make install DESTDIR=... PREFIX=...` installs both the executable and its bundled API.

Run `make test` for core and JSON-RPC integration tests. The implementation recognizes `class_name`, path- and UID-based `extends`, qualified script aliases, script resources versus scenes, inner classes, autoloads, typed containers and `:=` inference, inherited script/native members, callable and signal provenance, arbitrary member/call/subscript chains, and unsaved overlays. Ordinary `var value = expression` declarations remain dynamically typed, although their initializer is retained as a completion hint. Unsupported or ambiguous expressions degrade to `Variant`/unknown instead of consulting a running editor.

Run `make benchmark-completion` for informational warm-completion, body-edit, incomplete member-chain, and declaration-edit fallback latency distributions. It uses a small deterministic fixture by default; set `BENCHMARK_PROJECT=.` to measure the current repository at project scale. The benchmark reports p50, p95, and maximum timings without imposing a machine-dependent pass/fail threshold.

Diagnostics cover tree-sitter syntax failures, duplicate members and global classes, unresolved or cyclic inheritance, unknown explicit types, constants which are values rather than valid metatypes, incompatible typed initializers, source-ordered name resolution, statically known members and calls, argument arity/types, and typed return paths. Unsafe property, method, and call-argument access follows the corresponding `debug/gdscript/warnings/unsafe_*` values from `project.godot`, including Godot's disabled defaults and warn/error levels. Edited-file diagnostics are published promptly; dependency-aware background work refreshes only affected consumers and suppresses unchanged or initially empty notifications. Standard watched-file events and a cached mtime/size poll keep unopened disk files current without replacing open overlays. Dynamic `Variant`, `Dictionary`, node-path, and otherwise unresolved expressions deliberately remain unchecked to avoid speculative errors.

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

Set `GODOT=/path/to/godot` if the executable is not on `PATH`. `GDScriptLanguageService` indexes asynchronously and exposes LSP-shaped `completion`, `hover`, `definition`, `document_symbols`, `diagnostics`, `resolve_type`, and rich `resolve_expression` methods to GDScript. `update_document`, `close_document`, and `refresh_files` keep the shared index current without `GDScript` resources or editor services.

`completion_ex(uri, line, column, {"profile": "helpers"})` additionally returns `disposition` (`not_handled`, `augment`, or `replace`) and the responsible provider. The default `full` profile remains a complete standalone completion list. `set_configuration()` accepts the `gdscriptLsp` body shown above.

The adapter emits `workspace_ready`, `workspace_error`, `index_updated`, and `diagnostics_updated`; after an update, the latter contains the changed document and its transitive dependents rather than every indexed script. Callers should wait for readiness before querying. Copy `addons/gdscript_lsp` into a project to package the extension, native metadata, and platform libraries together.

### Godot editor completion bridge

When the Code Completions addon is installed, enable the optional **Standalone GDScript Language Service** editor plugin. It registers a priority-zero provider and defaults to `plugin/code_completion/native/mode = "helpers_first"`: native helpers replace or augment narrowly owned contexts, while an unhandled request falls through to Godot and the existing GDScript providers unchanged. `"replace"` uses the native service for the complete popup and `"disabled"` bypasses it.

The bridge indexes asynchronously and falls through while it is not ready. It synchronizes an edited buffer only on Godot's debounced completion request and only when the `CodeEdit` version changed. Godot's internal code completion must therefore remain enabled; its separate network LSP server does not need to be disabled.

## Architecture

- `src/core`: engine-neutral C++20 parser, symbol graph, inheritance resolver, type inference, and query API.
- `src/lsp`: LSP 3.17 JSON-RPC transport over stdin/stdout.
- `src/gdextension`: thin godot-cpp wrapper over the same `Workspace` API.
- `addons/gdscript_lsp/data`: reduced Godot 4.6 native class baseline.

The index is deliberately multi-pass: all scripts are parsed first, global/path/UID class identities are registered next, statically resolvable script constants and qualified aliases are linked, then base edges are settled and cycle-checked. Queries walk that completed graph, which removes the old parser's dependency on editor-created `GDScript` resources and file load order.
