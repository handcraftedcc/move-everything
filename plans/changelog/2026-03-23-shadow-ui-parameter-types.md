# Shadow UI Parameter Type Expansion Changelog

Date: 2026-03-23
Branch: codex/shadow-ui-parameter-additions

## Added Parameter Types

- `note`
  - Generates note options centrally in Shadow UI.
  - Supports `mode: single|multi` and optional `min_note` / `max_note`.

- `rate`
  - Generates musical division options from metadata.
  - Supports `include_bars`, `bars_mode`, `include_triplets`.
  - `bars_mode` supports `bars-simple` (`16, 8, 4, 2, 1`) and `bars-every` (`16..1`, default).
  - `1 bar` is sourced from bars mode; `1/1` is no longer emitted as a base rate value.
  - Options are emitted in musical-time order (slowest -> fastest).

- `wav_position`
  - Supports waveform marker preview while editing.
  - Supports `display_unit: percent|sec|s|ms`, `mode: position|start|end` (legacy `trim_front`/`trim_end` aliases), and `filepath_param` linking.

- `string`
  - Opens the shared on-screen keyboard text entry flow when edited.

- `canvas`
  - Adds a trigger-style parameter with configurable idle display formatting via `display_value_type`.

## Visibility Rules

- Added metadata-driven `visible_if` support for:
  - individual params
  - level/folder definitions
- Supported operators:
  - `equals`, `not_equals`
  - `gt` / `greater_than` / `greater`
  - `lt` / `smaller_than` / `smaller`
  - `truthy`, `falsey` / `falsy`

## Module Author Notes

- Prefer declaring type behavior in `capabilities.chain_params`.
- `ui_hierarchy` can reference those params and add `visible_if` rules for navigation.
- `wav_position.filepath_param` should point at an existing `filepath` param key.

## Verification

- Added test: `tests/shadow/test_shadow_param_type_expansion.sh`
- Existing focused shadow tests still pass (hierarchy child prefixes, trigger enum reset, filepath browser regressions).
