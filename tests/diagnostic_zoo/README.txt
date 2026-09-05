GDScript Diagnostic Zoo
=======================

Put this directory at the root of a Godot project (or copy the files into res://).

Files:
- diagnostic_zoo.gd       warning-heavy, parser-valid target
- tool_base.gd            @tool helper required for MISSING_TOOL
- empty.gd                zero-byte file for EMPTY_FILE
- parser_error_zoo.gd     parser-recovery errors
- analyzer_error_zoo.gd   semantic/type errors
- project.godot           enables warning categories normally ignored by Godot
- warning_codes.txt       current warning-code coverage checklist

As of 2026-09-04, Godot master defines 46 current GDScript warning enum entries,
plus 3 deprecated-gated 3.x entries in builds where deprecated APIs are enabled.
The fixture targets all 45 warnings that are currently producible: 44 in
`diagnostic_zoo.gd` plus `EMPTY_FILE` in the zero-byte `empty.gd`.

Four enum entries are currently not producible:
- DEPRECATED_KEYWORD: Godot docs say there are currently no deprecated keywords.
- PROPERTY_USED_AS_FUNCTION, CONSTANT_USED_AS_FUNCTION, FUNCTION_USED_AS_PROPERTY:
  source explicitly says these migrated from 3.x by mistake and are never produced.

Why multiple files?
- EMPTY_FILE is mutually exclusive with every non-empty warning case.
- MISSING_TOOL needs another @tool script as its base.
- Parser errors can prevent semantic analysis, so parser-recovery errors are kept
  away from the warning-coverage fixture.
- Parser/analyzer errors do not currently have a warning-like stable Code enum;
  the parser stores an error message and source range instead.

The project settings turn normally-ignored warning categories on and downgrade
warning-as-error defaults to Warn so the warning fixture can report broadly.
Some diagnostics are build-feature dependent. In particular, Unicode confusable
identifier reporting depends on Unicode security support.

Standalone coverage verified against Godot 4.6.3 on 2026-09-05
------------------------------------------------------------
Implemented warning categories: UNUSED_VARIABLE, UNUSED_LOCAL_CONSTANT,
UNUSED_PARAMETER, SHADOWED_VARIABLE, SHADOWED_VARIABLE_BASE_CLASS,
SHADOWED_GLOBAL_IDENTIFIER, UNREACHABLE_CODE, UNREACHABLE_PATTERN,
UNSAFE_VOID_RETURN, UNSAFE_PROPERTY_ACCESS, UNSAFE_METHOD_ACCESS,
and UNSAFE_CALL_ARGUMENT. The remaining warning categories are deferred.

The analyzer error zoo's four former gaps (operators, casts, void results,
and typed-array literal elements) are checked. Node2D = Node.new() is valid
statically; its old error expectation was corrected. Unicode identifiers are
accepted without duplicate syntax errors. The parser zoo now checks loop
control, parameter ordering, void type positions, standalone lambdas and yield;
unknown annotations and nested typed collections are deliberately deferred.

This remains a combined recovery fixture, not a count-based conformance test.
Isolated sources, exact diagnostics, valid counterparts, and selected Godot
warning comparisons live in ../diagnostic_cases.json. Run make test-diagnostics
and make test-conformance GODOT=/path/to/godot from the repository root.
The selected cases have been executed against 4.6.3; the full master-warning
checklist above remains source-derived and is not a claim of engine parity.
