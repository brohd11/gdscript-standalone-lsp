# Modular Completion Providers, Configuration, and `const_key`

## Summary

Introduce an engine-neutral, compile-time completion-provider framework and port `const_key` as its first standalone provider. Keep the existing completion implementation behind a legacy built-in provider for incremental migration. No runtime plugin loader or interpreted language is added.

The standalone executable will load global user defaults from `~/.gdscript-lsp.json`, followed by project overrides from `<project>/.gdscript-lsp.json`.

## Key Changes

- Add an ordered provider registry with:
  - Stable provider ID, supported profiles, and priority.
  - `NotHandled`, `Augment`, and `Replace` outcomes.
  - A constrained completion context exposing the document, caret context, enclosing class, and semantic queries without Godot types.
  - Profile flags so future annotation providers can run in standalone mode while being excluded from the Godot helpers profile.
- Register `constKeys` ahead of the existing completion logic. Wrap the current monolithic implementation as a final legacy provider; migrate its individual helpers separately later.
- Implement `constKeys`, disabled by default:
  - Activate only inside the string or `StringName` initializer of a `const`.
  - Generate deduplicated snake_case and uppercase variants from the constant name, script name, and inner-class path, matching the existing GDScript provider.
  - Return `Replace` with string completion items and provider metadata `constKeys`; otherwise return `NotHandled`.
  - Support both full standalone and Godot-helper profiles.
- Extend `CompletionConfig` and both explicit configuration adapters with `completion.constKeys`.
- Reserve provider metadata for future annotations:
  - Tags remain inline source annotations.
  - Configuration may enable or rename known compiled handlers and their prefix.
  - Arbitrary tag behavior is not configurable code.
  - Future `dict_key`, `arg_location`, and `import` providers are standalone-only; the existing GDScript tag providers remain authoritative inside Godot.

## Configuration

- Load configuration in this order:
  1. Built-in defaults.
  2. `~/.gdscript-lsp.json` (`%USERPROFILE%\.gdscript-lsp.json` on Windows).
  3. `<project>/.gdscript-lsp.json`.
  4. LSP `initializationOptions.gdscriptLsp`.
  5. Explicit CLI overrides.
  6. Later `workspace/didChangeConfiguration` updates for dynamic settings.
- Use the existing `gdscriptLsp` configuration schema; config files may contain either its body directly or the optional `gdscriptLsp` wrapper.
- Add `completion.triggerAfterSpace`; use its effective value when advertising completion trigger characters during initialization.
- Retain `--space-prefix` as a highest-precedence compatibility override and document the JSON setting as preferred.
- Treat config files as startup inputs. Changes require restarting/reconnecting; this is mandatory for trigger-character changes.
- Report malformed files or invalid values to stderr and ignore only the invalid layer/value. Unknown keys are ignored for forward compatibility.
- Do not make `Workspace` or the GDExtension read home-directory files implicitly. Godot continues using global EditorSettings and passes explicit configuration to the shared service.

Example:

```json
{
  "completion": {
    "triggerAfterSpace": true,
    "constKeys": true
  }
}
```

## Test Plan

- Provider pipeline tests covering stable ordering, all three dispositions, full/helper profile filtering, and fallback into the existing completion implementation.
- `constKeys` tests for global-class and filename-derived script names, nested classes, snake/uppercase variants, duplicate removal, string and `StringName` literals, disabled configuration, and rejection of variables or unrelated strings.
- Configuration tests covering user defaults, project overrides, initialization overrides, CLI precedence, missing files, malformed JSON, and invalid value types.
- LSP integration tests verifying config-driven space advertisement/filtering and serialized `constKeys` provider metadata.
- GDExtension smoke coverage for explicitly enabling `constKeys`, without implicit JSON loading.
- Run the complete existing core, provider, incremental, LSP, and GDExtension test suites to ensure completion ranking and current helpers remain unchanged.

## Assumptions

- This phase ports only `const_key`; annotation parsing and the tag-based providers come later.
- Providers are compiled into the core. The facade is designed to permit a future subprocess or WASM host, but no external plugin ABI is committed now.
- `constKeys` remains off by default to match the legacy addon.
- Project configuration overrides global preferences only for keys it explicitly defines.
