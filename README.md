# Standalone GDScript language server

An experimental language server for Godot 4.6 GDScript. It indexes projects
directly from disk, so the Godot editor does not need to be running.

It provides completion, signature help, hover information, go to definition,
document symbols, and diagnostics. 

Along with the standalone nature, some custom completions are included. For example, better enum suggestions.

Diagnostics are based off of Godot 4.6. Mostly non-parsable errors are targeted. An optional bridge can use Godot itself for
diagnostics while keeping the extended completions.

Completion is based off of Godot 4.6 as well. If you are on a different version, generate the extension_api via Godot and
place the file at: `res://.godot/addons/gdscript_parser/extension_api.json`

## Install

On macOS or Linux:

```sh
curl -fsSL https://raw.githubusercontent.com/brohd11/gdscript-standalone-lsp/main/install.sh | sh
```

On Windows, in PowerShell:

```powershell
irm https://raw.githubusercontent.com/brohd11/gdscript-standalone-lsp/main/install.ps1 | iex
```

See [installation](docs/install.md) for supported platforms, installer options,
and manual installation.

## Connect an editor

Configure your editor's LSP client to start `gdscript-lsp` over standard input
and output. The server discovers the Godot project from the workspace supplied
during LSP initialization.

VS Code's Godot Tools extension uses a TCP connection and needs a small amount
of extra setup. See [editor setup](docs/editors.md) for both connection modes.

## Documentation

- [Installation](docs/install.md)
- [Building from source](docs/build.md)
- [Editor setup](docs/editors.md)
- [Features and diagnostics](docs/features.md)
- [Configuration](docs/configuration.md)
- [Godot engine diagnostics](docs/engine-diagnostics.md)
- [GDExtension integration](docs/gdextension.md)
- [Development and architecture](docs/development.md)
