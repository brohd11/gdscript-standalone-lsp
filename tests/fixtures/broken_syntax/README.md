# Semantic recovery tests

`make test` and CTest's `semantic-recovery` test run this corpus through both the
core and the LSP. The fixture's small API keeps isolated test processes cheap;
the existing suites continue testing the bundled Godot API.

To reproduce one family or case:

```sh
make build/gdscript-lsp build/broken-syntax-tests
python3 tests/broken_syntax.py build/gdscript-lsp build/broken-syntax-tests --case declaration/function-open
```

`--case` matches substrings. `--jobs 1` runs sequentially. Each core/LSP process
has a ten-second deadline. Failures include the case, edit step, query result or
process error, and source; the runner continues to collect other failures.

## Adding cases

`cases.json` contains named `body` and `declaration` cases. Each has a `name`,
`broken` fragment, and `valid` repair. Body fragments are indented into a method
with a known Node local. Declaration fragments sit among valid methods.

- `receiver` requires Node completions at the broken caret, such as `n.`.
- `ambiguous_tail` explicitly permits apparent declarations after an unfinished
  string to become unavailable. Prefix lookups must still work, and closing the
  string must restore the whole document. This does not exempt normal syntax
  errors from preserving neighboring declarations.
- `sweep_seeds` are valid body fragments. The runner deterministically deletes
  structural punctuation one token at a time and restores it. It also tests
  every typing prefix of a member call and a function signature.

Selected named cases additionally cover EOF without a final newline, reversed
declaration order, inner classes, adjacent damaged functions, and CRLF/spaces.
All sources include Unicode before the query positions.

Every sequence opens broken text, repairs it, breaks and repairs it with ranged
edits, then repeats using a full replacement. Stable methods and locals have
explicit type, completion, ownership, hover, and definition expectations. Empty
completion lists cannot satisfy known-receiver probes. Diagnostics must not
leak into the stable methods.

The C++ driver independently compares clean and incremental syntax, symbol,
diagnostic, and semantic snapshots, including byte/UTF-16 range consistency.
The Python runner applies the same expectations to both paths, compares their
observations, and verifies that each repair restores the original valid
snapshot. The driver consumes generated JSON on stdin; use the Python runner
rather than invoking it directly.
