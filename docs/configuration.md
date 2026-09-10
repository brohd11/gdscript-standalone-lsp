# Configuration

Settings can be supplied under `initializationOptions.gdscriptLsp` and changed
later with `workspace/didChangeConfiguration`:

```json
{
  "gdscriptLsp": {
    "completion": {
      "enums": true,
      "extendedTypeHints": true,
      "constructors": true,
      "hidePrivate": true,
      "callableInsertStyle": "auto",
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

`completion.callableInsertStyle` controls how parameterized function and
constructor completions are inserted by the standalone LSP server:

- `auto` (the default) inserts a paired-parenthesis snippet with the caret
  inside when the client advertises snippet support, and otherwise inserts
  only the callable name.
- `name` always inserts only the callable name so typing `(` remains under the
  editor's control.
- `openParen` inserts a trailing `(` for clients that auto-pair parentheses
  added by a completion item.

Zero-argument callables continue to insert a complete `()` pair. The Godot
editor integration keeps its native trailing-`(` behavior. This setting can be
supplied during initialization or changed for subsequent completions with
`workspace/didChangeConfiguration`.

`diagnostics.pollIntervalMs` controls the disk-change poll used when a client
does not send file-watch notifications. Positive values are clamped to at least
100 ms; `0` disables polling.

## Completion after spaces

The `--space-prefix` option enables Godot-editor-style automatic completion
after one space following an existing completion prefix. This includes `= `,
`== `, `!= `, `<= `, `>= `, argument separators, opening parentheses, and
type-hint colons. Ordinary and repeated spaces are ignored, and explicit
completion requests remain unaffected.

Trigger characters are negotiated when the LSP session starts, so changing
this option requires reconnecting the client.

## Native API selection

The server searches for Godot API metadata in this order:

1. `--api /path/to/extension_api.json`
2. `GDSCRIPT_LSP_API=/path/to/extension_api.json`
3. API metadata generated for the target project
4. The bundled, reduced Godot 4.6 API snapshot

A project-specific snapshot can contain APIs provided by its GDExtensions, so
it takes precedence over the bundled baseline.

## Custom requests

Alongside standard LSP 3.17 requests, the server provides
`gdscript/resolveType`, `gdscript/resolveExpression`, and
`gdscript/documentSymbols`.

`gdscript/resolveExpression` accepts the same parameters as
`gdscript/resolveType` and returns `{ type, origin, accessPaths }`. The origin
identifies the declaration that produced the value. Access paths contain
caller-verified spellings in preferred order: preload alias, local or inherited,
then global.

`gdscript/documentSymbols` accepts standard document-symbol parameters and
returns `{ version, symbols }`. Its symbol tree adds declaration IDs, owners,
resolved and return types, origins, and semantic flags. Declaration IDs are
opaque and should not be parsed by clients.
