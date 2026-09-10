# Features and diagnostics

The server implements incremental document synchronization, completion,
completion-item resolve, signature help, hover, definition, document symbols,
and push and pull diagnostics.

## Semantic model

The language service understands global classes, path- and UID-based
inheritance, qualified script aliases, script and scene resources, inner
classes, autoloads, typed containers, `:=` inference, inherited script and
native members, callables, signals, member and subscript chains, and unsaved
editor buffers.

Unsupported or ambiguous expressions resolve conservatively to `Variant` or an
unknown type. An untyped `var value = expression` remains dynamically typed,
although its initializer can still inform completion.

## Completion

Completion results are ranked by lexical scope and nearest-first inheritance.
Instance members precede type-level members at each level. Class receivers put
`new` first and omit instance-only members.

Callable items use Godot's compact `name()` or `name(…)` presentation.
Parameterized calls insert an opening parenthesis so clients with bracket
pairing place the cursor inside the call. At class scope, completion after
`func ` or `static func ` offers methods that can still be overridden and
inserts the full signature and a `pass` body.

Portable helpers provide expected enum values, extended type-hint names,
expected-type constructors, and method or property names used as strings.
Private members remain hidden until the typed prefix starts with `_`.

Each semantic completion item includes an opaque declaration ID in
`data.gdscriptLsp`. Clients can pass the item to `completionItem/resolve` for
declaration-backed detail and documentation.

## Diagnostics

Diagnostics cover syntax and structural errors, duplicate declarations,
inheritance failures, unknown or invalid types, typed assignments and returns,
known members and calls, argument count and types, invalid operators and casts,
typed array operations, invalid constant expressions, missing or nonconstant
`preload()` paths, and invalid control flow. Constant preload paths may be built
from other string constants, concatenation, formatting, and safe `String()`
construction. The lexical pass also checks string escapes and indentation.

The server avoids speculative errors for dynamic `Variant`, `Dictionary`, node
paths, and unresolved expressions. File-watch notifications and a portable disk
poll keep unopened files current without replacing unsaved editor buffers.

Warnings include unused and shadowed declarations, unreachable statements,
covered match patterns, no-effect expressions, and unsafe void returns. Warning
levels and directory rules follow the GDScript settings in `project.godot`.

Use `@warning_ignore("unused_variable")` on a declaration or statement, or
`@warning_ignore_start(...)` and `@warning_ignore_restore(...)` around a region.
Suppression also applies when a warning is promoted to an error. Syntax and type
errors cannot be suppressed this way. Source warning names use underscores;
diagnostic codes use kebab-case.

## Compatibility

The language baseline is Godot 4.6 and conformance is tested against Godot
4.6.3. Newer, unmodeled versions validate known annotations while accepting
unknown annotation names. Older reduced API snapshots load conservatively and
omit operator checks when the required metadata is unavailable.

Full warning parity and typed-dictionary and mutating-container-method checks
are not yet implemented.
