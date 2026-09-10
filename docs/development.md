# Development

## Tests

Run the core and JSON-RPC integration tests with:

```sh
make test
```

Run isolated diagnostic cases, live LSP warning updates, and the parser coverage
matrix with:

```sh
make test-diagnostics
```

The diagnostic suite is documented in
[`tests/diagnostics/README.md`](../tests/diagnostics/README.md). Compare stable
rule categories and parser results with a Godot 4.6 editor executable using:

```sh
make test-conformance GODOT=/path/to/godot
```

The conformance tests use temporary projects and do not run the test scripts or
launch the debugger.

Run `make benchmark-completion` for cold and cached outline, warm completion,
body-edit, incomplete member-chain, and declaration-edit latency distributions.
Set `BENCHMARK_PROJECT=.` to measure this repository at project scale. The
benchmark reports p50, p95, and maximum timings without a machine-dependent
pass/fail threshold.

## Parser dependency

`make deps` checks out the pinned tree-sitter-gdscript dependency and applies a
tracked compatibility patch. The patch supports Godot-valid inline `if` lambda
bodies, fixes indentation across comment-only lines, and uses Godot 4.6's
Unicode 16 identifier ranges.

Generated parser artifacts and shared identifier tables are checked in, so
normal builds do not require the tree-sitter generator.

## Architecture

- `src/core` contains the engine-neutral C++20 parser, symbol graph, inheritance
  resolver, type inference, and query API.
- `src/lsp` implements LSP 3.17 JSON-RPC over standard input and output, plus
  loopback TCP adapters for POSIX and Windows.
- `src/gdextension` wraps the shared `Workspace` API with godot-cpp.
- `addons/gdscript_lsp/data` contains the reduced Godot 4.6 native class
  baseline.

Indexing uses several passes: scripts are parsed, global/path/UID class
identities are registered, script constants and qualified aliases are linked,
and inheritance edges are resolved and checked for cycles. Queries run against
the completed graph, independent of editor-created `GDScript` resources and
file load order.

During editing, damaged function blocks are reparsed within lexical boundaries.
Recovered signatures and body syntax feed the normal symbol and semantic
queries, while the original whole-document tree remains available for
incremental edits.

## Releases

Push a version tag on a commit containing the release workflow:

```sh
git tag v0.1.0
git push origin v0.1.0
```

Every `v*` tag builds and tests macOS, Linux, and Windows packages, validates the
extracted archives, and publishes a GitHub release with generated notes and
checksums. Tags containing a hyphen, such as `v0.2.0-rc.1`, become prereleases.
A failed workflow can resume an incomplete draft; an already published release
is left unchanged. The workflow uses the built-in `GITHUB_TOKEN`.
