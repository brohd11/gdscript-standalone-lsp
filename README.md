# Standalone GDScript language service

This repository contains an experimental Godot 4.6 GDScript semantic core and LSP server. It indexes a project directly from disk; the Godot editor does not need to be running.

```sh
make deps
make
build/gdscript-lsp
```

The Makefile remains the shortest Unix build path. A pinned CMake build is also
available for Linux, macOS, and Windows:

```sh
cmake -S . -B build-cmake
cmake --build build-cmake --config Release
ctest --test-dir build-cmake -C Release --output-on-failure
```

On Windows, run those commands from a Visual Studio developer shell (or let
CMake select the installed Visual Studio generator). Running `cmake --install build-cmake --config Release --prefix dist`
produces a self-contained layout with the executable and bundled Godot API
metadata. Semantic GDScript addon tests remain a manual editor-console check.

### Prebuilt releases

GitHub Releases provides standalone server packages for macOS 14 or newer on
Apple Silicon (`macos-arm64`), Linux x64 (`linux-x64`, built on Ubuntu 22.04),
and Windows x64 (`windows-x64`). macOS and Linux downloads are `.tar.gz` archives;
Windows downloads are `.zip` archives. Each release includes `SHA256SUMS` for
verifying the downloads.

Install the latest release into `~/.local/bin` on macOS or Linux with:

```sh
curl -fsSL https://raw.githubusercontent.com/brohd11/gdscript-standalone-lsp/main/install.sh | sh
```

On Windows, run this in PowerShell:

```powershell
irm https://raw.githubusercontent.com/brohd11/gdscript-standalone-lsp/main/install.ps1 | iex
```

The installers verify the release checksum and place the required API metadata
and documentation under `~/.local/share`. Set `VERSION=v0.1.0` in the Unix environment or
`$env:VERSION = 'v0.1.0'` in PowerShell to install a specific release. `BIN_DIR`
overrides the executable directory; its parent is treated as the install prefix.
If `~/.local/bin` is not already on `PATH`, an interactive installer offers to
add it. Pass `--modify-path` or `--no-modify-path` to `install.sh`; because `iex`
cannot receive arguments, PowerShell flags use the script-block form:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/brohd11/gdscript-standalone-lsp/main/install.ps1))) -ModifyPath
```

Extract the entire archive and point your editor at `bin/gdscript-lsp` (or
`bin/gdscript-lsp.exe` on Windows) inside the extracted directory. Keep `share/`
beside `bin/`: it contains the bundled Godot API metadata and documentation.
You can also add the extracted `bin/` directory to `PATH`. Packages contain the
standalone server; the GDExtension addon is built separately as described below.
macOS packages are unsigned and are not notarized.

To publish a release, push a version tag on a commit containing the release
workflow, for example:

```sh
git tag v0.1.0
git push origin v0.1.0
```

Every `v*` tag push builds and tests all three platforms, tests the extracted
packages, and then publishes a GitHub release with generated notes, the three
archives, and checksums. Tags containing a hyphen, such as `v0.2.0-rc.1`, publish
as prereleases. Rerunning a failed workflow can resume an incomplete draft;
an already published release is left unchanged. The workflow uses the built-in
`GITHUB_TOKEN` and requires no additional release secrets.

The server communicates over standard input/output using LSP 3.17. An editor should launch it without project arguments; the server selects and indexes the Godot project from `workspaceFolders` or `rootUri` during the standard `initialize` request. `--project /path/to/project` remains available for fixed-root integrations. One server process serves one Godot project.

### VS Code Godot Tools TCP adapter

The Godot Tools extension for VS Code expects the language server to listen on a TCP port. On Linux, macOS, and Windows, the optional TCP adapter exposes the same standalone server on the IPv4 loopback interface:

```sh
build/gdscript-lsp --tcp 6010 --space-prefix
```

The adapter stays running and starts a fresh, isolated LSP session for each connection, including concurrent clients. Standard input/output remains the default transport. Port `6010` is recommended to avoid Godot's usual editor, LSP, DAP, debugger, and legacy Godot Tools ports; another available port can be used if needed.

Disable Godot Tools' headless LSP mode and point it at the adapter in VS Code's `settings.json`:

```json
{
  "godotTools.lsp.headless": false,
  "godotTools.lsp.serverHost": "127.0.0.1",
  "godotTools.lsp.serverPort": 6010
}
```

Start the adapter before opening VS Code, or use the extension's retry action after starting it. A Godot editor can still run separately for debugging and other editor-backed features; leave its own LSP server on a different port. The adapter only exposes the capabilities advertised by this standalone server. `--project`, `--api`, and the optional `--space-prefix` completion behavior may be combined with `--tcp` as shown above.

It implements incremental document synchronization, completion, completion-item resolve, hover, definition, document symbols, push and pull diagnostics, and the custom `gdscript/resolveType`, `gdscript/resolveExpression`, and `gdscript/documentSymbols` requests.

Standard document symbols include best-known types in `detail` and expose each physical inner class exactly once as a separate outline root. `gdscript/documentSymbols` accepts standard document-symbol parameters and returns `{ version, symbols }`; its cached symbol tree adds declaration IDs, owners, resolved types, return types, origins, and semantic flags. The GDExtension `document_symbols()` method exposes the same rich fields.

Callable completions follow Godot's compact presentation: `name()` for zero arguments and `name(…)` otherwise. Parameterized calls insert a trailing `(` so clients with bracket pairing place the caret inside the call. Completion results are ranked by lexical scope and nearest-first inheritance, with type-level members behind instance members at each level; class receivers put `new` first and omit instance-only members. The array order and LSP `sortText` carry the same ranking.

Declaration names and completed statement/control-flow keywords suppress ordinary semantic completion. After `func ` or `static func ` at class scope, completion instead lists not-yet-overridden script methods and native virtual methods with matching staticness; selecting one inserts its full signature and an indented `pass` body.

Every semantic completion item carries an opaque declaration ID in `data.gdscriptLsp`, along with its provider and access-path kind. Clients may pass the item to standard `completionItem/resolve` for declaration-backed detail and documentation. `gdscript/resolveExpression` accepts the same parameters as `gdscript/resolveType`, but returns `{ type, origin, accessPaths }`: `origin` identifies the member, local, or native API declaration that produced the value, and `accessPaths` lists caller-verified spellings in preferred order (preload alias, local/inherited, then global). Clients should treat these IDs as opaque.

Portable completion helpers add expected enum values, extended type-hint names, expected-type constructors, and method/property names used as strings. Private members are hidden from ordinary member completion until the typed prefix starts with `_`. The helpers are built into the shared core, so they behave the same way over stdio and through the GDExtension.

`--space-prefix` opts into Godot-editor-style automatic completion after one space following an existing completion prefix, including `= `, `== `, `!= `, `<= `, `>= `, argument separators, opening parentheses, and type-hint colons. Ordinary and repeated spaces are ignored, and explicit completion requests such as Ctrl+Space are unaffected. Because trigger characters are negotiated when the LSP session starts, changing this option requires reconnecting the client. Omit the flag to avoid sending completion requests after spaces.

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

A project-specific snapshot can include APIs contributed by its GDExtensions, so it takes precedence over the baseline. `make install DESTDIR=... PREFIX=...` and `cmake --install` both install the executable and its bundled API.

Run `make test` for core and JSON-RPC integration tests. The implementation recognizes `class_name`, path- and UID-based `extends`, qualified script aliases, script resources versus scenes, inner classes, autoloads, typed containers and `:=` inference, inherited script/native members, callable and signal provenance, arbitrary member/call/subscript chains, and unsaved overlays. Ordinary `var value = expression` declarations remain dynamically typed, although their initializer is retained as a completion hint. Unsupported or ambiguous expressions degrade to `Variant`/unknown instead of consulting a running editor.

Run `make benchmark-completion` for informational cold/cached outline, warm-completion, body-edit, incomplete member-chain, and declaration-edit fallback latency distributions. It uses a small deterministic fixture by default; set `BENCHMARK_PROJECT=.` to measure the current repository at project scale. The benchmark reports p50, p95, and maximum timings without imposing a machine-dependent pass/fail threshold.

Diagnostics cover syntax and structural errors, duplicate members and global classes, unresolved or cyclic inheritance, unknown explicit types, invalid metatypes, incompatible typed initializers, source-ordered name resolution, statically known members and calls, instance members accessed through class references, argument arity/types, and typed return paths. Additional checks catch invalid arithmetic/bitwise operands and casts, use of void results, built-in types used as values, and incompatible array-literal elements and indexed writes to typed arrays. Structural checks cover loop-control placement, required parameters after defaults, invalid void type positions, standalone lambdas, incomplete declaration names and member access, removed Godot 3 keywords, duplicate parameters/enums/class headers/locals, invalid class-body statements and assignments, constructor/static misuse, nested typed collections, and invalid match bindings. A lexical pass validates string escapes and mismatched dedents, while targeted recovery checks diagnose malformed function and enum headers. Edited-file diagnostics are published promptly; dependency-aware background work refreshes only affected consumers and suppresses unchanged or initially empty notifications. Standard watched-file events and a cached mtime/size poll keep unopened disk files current without replacing open overlays. Dynamic `Variant`, `Dictionary`, node-path, and otherwise unresolved expressions deliberately remain unchecked to avoid speculative errors.

Unused locals, local constants and parameters, shadowed members/inherited members/global identifiers, unreachable statements and catch-all-covered match patterns default to Warning. Standalone no-effect expressions and standalone ternaries also default to Warning; assignments, calls and `await` remain valid expression statements. Unsafe void returns defaults to Warning, while unsafe property, method and call-argument access retain their disabled defaults. All implemented warnings respect `debug/gdscript/warnings/enable`, per-category levels (`0` Ignore, `1` Warning, `2` Error), and directory rules in `project.godot` (including the default exclusion of `res://addons`). File-watch notifications and polling refresh these settings without restarting the service.

Suppress warnings with `@warning_ignore("unused_variable")` on a statement/declaration, or use `@warning_ignore_start(...)` and `@warning_ignore_restore(...)` for a region. Suppression applies to warnings promoted to errors; syntax and type errors remain errors. Unknown warning names are ignored. Source warning names use Godot's underscore spelling; diagnostic codes use kebab-case.

The language baseline is Godot 4.6, with conformance tested against 4.6.3. Loading another API snapshot changes native symbols and supplied operator signatures, not language rules. Reduced schema version 3 preserves builtin operator tables. Older reduced snapshots still load and conservatively omit operator checks when that metadata is absent; the service never fills those gaps using another version's table. Unresolved expressions do not become proof of an error. Full warning parity, annotation semantics, and typed-dictionary/mutating-container-method checks remain outside this milestone; annotation gaps are enumerated in `tests/diagnostics/parse_errors.json`.

The pinned tree-sitter-gdscript dependency receives a tracked compatibility patch during `make deps`. The patch adds Godot-valid inline `if` lambda bodies, fixes indentation across comment-only lines, and uses Godot 4.6's Unicode 16 identifier ranges. Dependency setup verifies the exact upstream commit and upgrades checkouts carrying the previous compatibility patch. Generated parser artifacts and the shared identifier tables are checked in; normal builds do not require the tree-sitter generator.

To compare stable rule categories with the real Godot 4.6 parser, run:

```sh
make test-conformance GODOT=/path/to/godot
```

Run `make test-diagnostics` for isolated exact-set cases, live LSP warning updates, and the 99-source parser coverage matrix without Godot. The suite lives in `tests/diagnostics/`; `cases.json` pins exact diagnostics and `parse_errors.json` records errors, valid sources, and known annotation gaps. The conformance command checks both matrices against Godot, uses temporary projects, recognizes script errors even when Godot exits successfully, and isolates warnings by promoting one category at a time to Error. It does not launch the debugger or execute the test scripts. See `tests/diagnostics/README.md` for suite details and `THIRD_PARTY_NOTICES.md` for Godot attribution.

### Optional engine diagnostics

Use a Godot **editor executable** to obtain that engine's own LSP diagnostics while keeping this server's completion, navigation, outline and type resolution:

```sh
# Launch and own a headless editor for the selected project.
build/gdscript-lsp --godot /path/to/godot

# Attach to the LSP of an editor already running the same project.
build/gdscript-lsp --godot-lsp-port 6005
```

Both options work with stdio or the existing `--tcp` adapter. Attachment connects to `127.0.0.1` and rejects a different project. The frontend TCP port must differ from Godot's LSP port. Launch mode allocates dedicated LSP, DAP and debugger ports and owns one headless editor per frontend session; closing the session terminates its owned engine. Attachment never terminates the existing editor. On macOS, supply the executable inside the application bundle, such as `Godot.app/Contents/MacOS/Godot`.

Alternatively, configure the backend through `initializationOptions` or `workspace/didChangeConfiguration`:

```json
{
  "gdscriptLsp": {
    "diagnostics": {
      "engine": {
        "mode": "launch",
        "executable": "/path/to/godot"
      }
    }
  }
}
```

The default mode is `"off"`. Attach mode uses `{"mode":"attach","port":6005}`. `--godot` and `--godot-lsp-port` are mutually exclusive and take precedence over engine configuration. Completion configuration and native API snapshot selection remain independent of the engine bridge.

While connected, engine diagnostics replace the standalone set. Numeric codes, messages, severity, ranges and optional diagnostic fields are preserved, including engine quirks; standalone warning settings and suppression logic are not reapplied. Full-text unsaved buffers are sent after a 150 ms debounce, with explicit diagnostic pulls prioritized immediately. Edits invalidate pending results, and synchronization barriers prevent unversioned engine reports from being attributed to newer edits. Push and pull diagnostics use the same accepted results. Unopened scripts are checked in the background using temporary upstream document opens.

Startup and connection failures use current standalone diagnostics. Startup allows up to 60 seconds for imports; diagnostic transactions time out after five seconds. Reconnection uses exponential backoff capped at 30 seconds and replays current overlays. During a healthy connection, the previous publication retains its original document version until a new engine result arrives. Completion requests do not wait for engine I/O.

Request `gdscript/diagnosticBackend` with empty parameters to inspect `mode`, `state`, `reason` and `engineVersion`. The same object is emitted through `gdscript/diagnosticBackendChanged`. States are `off`, `connecting`, `ready`, `fallback` and `reload-required`; attached engines may have an unknown (`null`) version. `gdscript/reconnectDiagnosticEngine` restarts an owned engine or reconnects an attachment. Changing engine configuration replaces the connection. Invalid runtime configuration is reported through `window/logMessage` and leaves the previous engine configuration in place.

Managed launch uses normal editor initialization, including imports, tool scripts, editor plugins and GDExtensions. Godot may update project caches and generated metadata. The bridge never saves unsaved source buffers. Changes to `project.godot` restart a managed editor; an attachment falls back until you reload the project in Godot and request reconnection. A managed engine's bind interface follows Godot's editor settings; the bridge itself connects only over loopback.

Parity means matching the connected **Godot LSP**, rather than adding compiler behavior its LSP does not expose. In particular, Godot 4.6.3's analyzer can resolve dependencies from disk even when another dependency buffer has unsaved edits. Upstream diagnostic ranges and duplicate reports are retained. Godot 4.6.3 is verified; other versions with the required full-text synchronization and document-symbol support are best effort. The embedded `GDScriptLanguageService` remains standalone.

`make test-engine-bridge` runs the fake-engine synchronization, fallback and lifecycle suite (also included in `make test` and CTest). Run `make test-engine-bridge-godot GODOT=/path/to/godot` for direct-versus-bridged diagnostic comparisons and managed launch/restart checks in disposable projects.

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
- `src/lsp`: LSP 3.17 JSON-RPC over stdin/stdout, with optional POSIX and Windows loopback TCP adapters.
- `src/gdextension`: thin godot-cpp wrapper over the same `Workspace` API.
- `addons/gdscript_lsp/data`: reduced Godot 4.6 native class baseline.

The index is deliberately multi-pass: all scripts are parsed first, global/path/UID class identities are registered next, statically resolvable script constants and qualified aliases are linked, then base edges are settled and cycle-checked. Queries walk that completed graph, which removes the old parser's dependency on editor-created `GDScript` resources and file load order.

While editing, damaged function blocks are reparsed within lexical boundaries. Recovered signatures and body syntax feed the same symbol and semantic queries, so an incomplete expression cannot absorb a later function's return type or locals. The original whole-document tree remains available for incremental edits.
