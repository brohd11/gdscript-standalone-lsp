# Diagnostics tests

This directory is the single home for diagnostic regressions and Godot
conformance data.

- `cases.json` and `cases.py` assert exact LSP diagnostic sets for isolated
  parser, semantic, warning, range, and overlay cases.
- `updates.py` checks push/pull consistency, overlays, and live settings changes.
- `parse_errors.json` and `parse_errors.py` keep the 99-source Godot 4.6.3 audit
  executable. `error` entries must produce an LSP error, `ok` entries must stay
  silent, and any future `gap` entries deliberately fail if they stop being
  silent before the matrix is updated.
- `oracle.json` and `oracle.py` compare stable fixture and warning cases with a
  real Godot executable.
- `fixtures/errors/` and `fixtures/warnings/` contain the projects used by the
  C++ and Python suites.
- `warning_coverage.md` catalogs warning names consumed by the oracle and states
  the standalone implementation status; it does not claim unexecuted parity.

Run `make test-diagnostics` without Godot. Run
`make test-conformance GODOT=/path/to/Godot` for the engine-backed checks.

The annotation specification table covers the complete Godot 4.6.3 registry,
generic placement/argument validation, and annotation-specific export,
`@onready`, warning-region, and RPC rules. The parse-error audit currently has
no known gaps.
