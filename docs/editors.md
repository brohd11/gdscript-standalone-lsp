# Editor setup

## Standard input and output

Most LSP clients should launch `gdscript-lsp` without project arguments and
communicate over standard input and output. The server selects and indexes the
Godot project from `workspaceFolders` or `rootUri` during the `initialize`
request. One server process serves one Godot project.

For integrations that cannot supply a workspace, set a fixed root explicitly:

```sh
gdscript-lsp --project /path/to/project
```

## VS Code with Godot Tools

The Godot Tools extension expects a TCP language server. Start the adapter on a
free loopback port; `6010` avoids Godot's usual editor and debugging ports:

```sh
gdscript-lsp --tcp 6010 --space-prefix
```

Then disable Godot Tools' headless LSP mode and point it at the adapter in VS
Code's `settings.json`:

```json
{
  "godotTools.lsp.headless": false,
  "godotTools.lsp.serverHost": "127.0.0.1",
  "godotTools.lsp.serverPort": 6010
}
```

Start the adapter before opening VS Code, or use the extension's retry action
after starting it. A Godot editor can run separately for debugging and other
editor-backed features as long as its LSP server uses a different port.

The TCP adapter starts an isolated LSP session for each connection and supports
concurrent clients. `--project`, `--api`, and `--space-prefix` can be combined
with `--tcp`. See [configuration](configuration.md) for details.
