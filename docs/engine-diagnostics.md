# Godot engine diagnostics

The optional engine bridge uses a Godot editor executable for diagnostics while
the standalone server continues to provide completion, navigation, symbols,
and type resolution.

## Start or attach

Launch a headless editor owned by the language server:

```sh
gdscript-lsp --godot /path/to/godot
```

Or attach to an editor already running the same project:

```sh
gdscript-lsp --godot-lsp-port 6005
```

Both modes work over standard input and output or with the `--tcp` adapter. The
front-end TCP port must differ from Godot's LSP port. On macOS, provide the
executable inside the application bundle, such as
`Godot.app/Contents/MacOS/Godot`.

Launch mode allocates dedicated LSP, DAP, and debugger ports and owns one
headless editor per front-end session. Closing the session terminates that
editor. Attach mode connects over loopback and never terminates the existing
editor; it rejects an editor opened on a different project.

## Configure through LSP

The bridge can also be configured through `initializationOptions` or
`workspace/didChangeConfiguration`:

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

The default mode is `"off"`. Attach mode uses
`{"mode":"attach","port":6005}`. Command-line options take precedence over
LSP configuration, and `--godot` and `--godot-lsp-port` are mutually exclusive.

## Behavior

While connected, engine diagnostics replace standalone diagnostics. The bridge
preserves the engine's codes, messages, severity, ranges, optional fields, and
duplicate reports. Standalone warning settings and suppressions are not applied
to engine results.

Unsaved buffers are sent after a 150 ms debounce. Explicit diagnostic pulls are
prioritized, and synchronization barriers prevent older engine reports from
being attributed to newer edits. Unopened scripts are checked in the background
using temporary upstream document opens.

Startup can take up to 60 seconds while Godot imports a project. Diagnostic
transactions time out after five seconds. Connection failures fall back to
standalone diagnostics, and reconnection uses exponential backoff capped at 30
seconds before replaying current buffers. Completion does not wait for engine
I/O.

Managed launch performs normal editor initialization, including imports, tool
scripts, editor plugins, and GDExtensions. Godot may update project caches and
generated metadata, but the bridge never saves unsaved source buffers. A change
to `project.godot` restarts a managed editor. An attached editor falls back until
the project is reloaded and reconnection is requested.

Godot 4.6.3 is verified. Other versions that support the required full-text
synchronization and document-symbol behavior are best effort.

## Status and recovery

Request `gdscript/diagnosticBackend` with empty parameters to inspect `mode`,
`state`, `reason`, and `engineVersion`. The same object is emitted through
`gdscript/diagnosticBackendChanged`. Possible states are `off`, `connecting`,
`ready`, `fallback`, and `reload-required`.

Use `gdscript/reconnectDiagnosticEngine` to restart an owned editor or reconnect
an attachment. Invalid runtime configuration is reported through
`window/logMessage` and leaves the previous configuration active.

Run `make test-engine-bridge` for the fake-engine synchronization, fallback, and
lifecycle suite. For direct comparisons against Godot, run:

```sh
make test-engine-bridge-godot GODOT=/path/to/godot
```
