# Installation

Prebuilt releases are available for macOS 14 or newer on Apple Silicon,
Linux x64, and Windows x64. Each release includes checksums.

## Install script

On macOS or Linux, the installer places `gdscript-lsp` in `~/.local/bin`:

```sh
curl -fsSL https://raw.githubusercontent.com/brohd11/gdscript-standalone-lsp/main/install.sh | sh
```

On Windows, run the PowerShell installer:

```powershell
irm https://raw.githubusercontent.com/brohd11/gdscript-standalone-lsp/main/install.ps1 | iex
```

The installers verify the release checksum and place the bundled Godot API
metadata and documentation under `~/.local/share`.

Set `VERSION=v0.1.0` in the Unix environment or
`$env:VERSION = 'v0.1.0'` in PowerShell to install a specific release. Set
`BIN_DIR` to change the executable directory; its parent becomes the install
prefix.

If `~/.local/bin` is not on `PATH`, an interactive installation offers to add
it. On Unix, pass `--modify-path` or `--no-modify-path`. To pass the equivalent
option through PowerShell, use a script block:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/brohd11/gdscript-standalone-lsp/main/install.ps1))) -ModifyPath
```

## Manual installation

Download an archive from
[GitHub Releases](https://github.com/brohd11/gdscript-standalone-lsp/releases)
and extract the entire archive. Point your editor at `bin/gdscript-lsp`, or
`bin/gdscript-lsp.exe` on Windows. Keep `share/` beside `bin/`; it contains the
required Godot API metadata and documentation.

macOS packages are unsigned and are not notarized. Release packages contain the
standalone server; build the GDExtension separately if you need the embedded
integration. See [building from source](build.md) for local builds.
