# Shadow UI Parameter Type Expansion Changelog

Date: 2026-03-23
Branch: parameter-additions

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
  - Adds a custom canvas parameter with configurable display formatting via `display_value_type`.

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

---

## Update: 2026-03-24 (Canvas Overlay Port)

### Canvas Behavior Update

- Ported the `canvas` implementation to a dedicated fullscreen canvas view (`VIEWS.CANVAS`) in `src/shadow/shadow_ui.js`.
- Canvas opens on click from hierarchy (`Push: open`) instead of triggering a hardcoded parameter write.
- Canvas scripts are now loaded from module scope using `canvas_script` (default `canvas.js`, supports `file.js#overlay_name` selector).
- Named overlay lookup supports `canvas_overlay` (and aliases `canvas_target` / `overlay`).
- Runtime overlay hooks are supported: `onOpen`, `onMidi`, `tick`, `draw`, `onClose`, `onExit`.
- Canvas footer now shows:
  - left: current parameter value
  - right: parameter/file label
- Background overlays are skipped while canvas is active so the canvas UI renders on a clean black background.
- Added optional footer visibility metadata:
  - `show_footer` (alias `showfooter`) defaults to `true`
  - set to `false` for fully clean canvas rendering

### Trigger Logic Removal

- Removed trigger-style canvas handling from hierarchy knob/edit flow.
- `canvas` no longer sets `"trigger"` in response to turns/clicks; value display remains available.

### Docs + Test Updates

- Updated `docs/MODULES.md` canvas type docs to describe `canvas_script` / `canvas_overlay` overlay workflow (replacing trigger-style docs).
- Updated `tests/shadow/test_shadow_param_type_expansion.sh` assertions from legacy `triggerCanvasParam` to:
  - `openCanvasPreview`
  - `drawCanvasPreview`
  - `dispatchCanvasMidi`
  - canvas-view transition expectations

### Verification (2026-03-24)

- `bash tests/shadow/test_shadow_param_type_expansion.sh` PASS
- `bash tests/shadow/test_shadow_hierarchy_child_prefix.sh` PASS
- `bash tests/shadow/test_shadow_trigger_enum_reset.sh` PASS

### Install Status

- Built and installed from `parameter-additions` using:
  - `./scripts/build.sh`
  - `./scripts/install.sh local --skip-confirmation --skip-modules`
- `--skip-modules` used to avoid touching separately installed module repos.
