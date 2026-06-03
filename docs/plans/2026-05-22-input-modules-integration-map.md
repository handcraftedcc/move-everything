# Input Modules Integration Map

**Date:** 2026-05-22

**Requirements:** `docs/plans/2026-05-22-schwung_input_modules_developer_requirements.md`

## Existing Systems To Reuse

### Module Discovery

Reuse `src/host/module_manager.c` and existing `module.json` parsing. It already reads `id`, `name`, `version`, `ui`, `dsp`, `api_version`, `component_type`, `defaults`, and common capabilities.

Needed extension:

- recognize `component_type: "input_module"` in host/UI lists
- optionally expose `ui_input` metadata later for custom input UI
- keep the 8 KB `module.json` limit

### Shadow UI Parameter Editor

Reuse the hierarchy editor in `src/shadow/shadow_ui.js`, especially:

- `getComponentHierarchy`
- `enterHierarchyEditor`
- hierarchy level/param loading
- knob context and parameter formatting
- `Swap module` action behavior already used for chain components

Needed extension:

- add an input-module context that supplies `get_param`/`set_param` shims for the active track's selected input module
- inject `Swap Input Module` as a final action row
- add an input picker view similar to component selection, but scoped to `input_module`

### Set State

Reuse the existing set-change signal and per-set state directory:

- set UUID/name cache: `src/host/shadow_set_pages.c`
- UI flag: `SHADOW_UI_FLAG_SET_CHANGED`
- shadow UI set reload path: `src/shadow/shadow_ui.js`
- state root: `/data/UserData/schwung/set_state/<set_uuid>/`

Needed extension:

- add `input_modules.json`
- load/save in the shadow UI thread, not in the SPI path
- initialize missing tracks to `native`

### MIDI Injection

Reuse the existing MIDI injection SHM:

- structure: `shadow_midi_inject_t` in `src/host/shadow_constants.h`
- JS binding precedent: `move_midi_inject_to_move` in `src/shadow/shadow_ui.c`
- C queue helper: `shadow_chain_midi_inject` in `src/host/shadow_midi.c`
- drain path: `shadow_drain_midi_inject`

Needed extension:

- add a named C wrapper such as `shadow_input_emit_cable2(const uint8_t packet4[4])`
- validate cable/CIN/status/data before queueing
- keep drain deferral/rate-limit behavior unchanged

Stage 2 implementation note:

- `src/host/shadow_input_modules.c` reuses `shadow_chain_midi_inject` as the
  cable-2 emit path and validates module output before queueing it.

### MIDI Interception

The pad blocking precedent is in `src/schwung_shim.c`:

- `shadow_control->pad_block`
- MIDI_IN copy/filter loop that zeroes pad notes 68-99
- post-ioctl hardware MIDI_IN scan for shortcuts and sampler triggers
- pad event forwarding to shadow UI when `pad_block` is enabled

Best hook:

- intercept in the same post-ioctl MIDI_IN filtering phase where the shim builds the shadow MIDI_IN buffer from hardware MIDI_IN
- process only cable 0 note-on/note-off for pads 68-99 in Stage 2
- zero the event in `shadow` only when the input module returns `handled = 1`

Do not write into `hardware_mmap_addr` except in existing shortcut code paths that already do so intentionally.

### Active Track

Reuse `shadow_control->selected_slot` and existing track button handling in `src/schwung_shim.c`.

Track buttons are reversed:

```text
CC43 = Track 1
CC42 = Track 2
CC41 = Track 3
CC40 = Track 4
```

Needed extension:

- input runtime tracks active track generation
- on change, release generated notes, swap/suspend active input instance, clear LED ownership
- initialize each track's input octave from `uiOctaveIndex` in the active set
  `Song.abl`; observe Up/Down CCs for live octave changes without blocking
  those native buttons

### Move UI Mode

Existing state:

- `shadow_control->move_ui_mode`
- D-Bus screen-reader parsing in `src/host/shadow_dbus.c` currently sets session and set overview
- track button press sets note mode in `src/schwung_shim.c`

Needed extension:

- add `src/host/move_mode_watcher.c/.h`
- poll Sentry breadcrumbs at 100-250 ms
- publish cached mode and generation
- SPI path reads only cached enum/generation

Stage 2 implementation note:

- `move_mode_watcher` runs as a background thread and updates
  `shadow_control->move_ui_mode`; the pad path only reads the cached value.

### LED Queue And Restore

Reuse `src/host/shadow_led_queue.c`:

- Move LED state cache
- overtake snapshot/restore
- rate-limited pending note/CC queues
- `shadow_queue_led`
- `led_queue_get_note_led_color`
- pad snapshot exposed to shadow UI

Needed extension:

- add pad-only ownership state for input modules
- snapshot/restore notes 68-99 only
- block native pad LED packets only when `replace_pads` is active
- leave step, track, transport, button, and knob LEDs native

Implemented 2026-05-22:

- `led_queue_set_input_pad_owner()` snapshots/restores pad notes 68-99 through
  the existing pending LED queue.
- `led_queue_set_input_pad_led()` and `led_queue_get_input_pad_led()` back the
  input module pad LED callbacks. `get_track_color()` is backed by parsed
  `Song.abl` top-level `tracks[].color` because MIDI LED capture is not a
  reliable source for track colors.
- True Chromatic uses the callbacks to draw root/in-scale/off-scale LEDs.

## New Internal Files

- `src/host/input_module_api_v1.h`
- `src/host/shadow_input_modules.c`
- `src/host/shadow_input_modules.h`
- `src/host/move_mode_watcher.c`
- `src/host/move_mode_watcher.h`
- `src/shadow/shadow_ui_input_modules.mjs`

## New Test Modules

- `src/modules/inputs/input-native/module.json`
- `src/modules/inputs/input-param-lab/module.json`
- `src/modules/inputs/true-chromatic-input/module.json`
- `src/modules/inputs/true-chromatic-input/dsp/input_plugin.c`

## Shortcut Hook

Add Step 9 to the existing Shift+Volume+Step shortcut section in `src/schwung_shim.c`.

Step notes are 16-31, so Step 9 is note 24. Confirm against hardware before finalizing.

Add a new UI flag in `src/host/shadow_constants.h`, for example:

```c
#define SHADOW_UI_FLAG_JUMP_TO_INPUT_MODULES 0x40
```

Then shadow UI consumes the flag and enters the input module screen for `shadow_get_selected_slot()`.

## Persistence Shape

```json
{
  "version": 1,
  "tracks": [
    { "track": 0, "module": "native", "params": {}, "led_mode": "native" },
    { "track": 1, "module": "true-chromatic-input", "params": {}, "led_mode": "native" },
    { "track": 2, "module": "native", "params": {}, "led_mode": "native" },
    { "track": 3, "module": "native", "params": {}, "led_mode": "native" }
  ]
}
```

## Risks And Unknowns

- Step 9 is inferred as note 24 from the documented step range and existing Step 2/13 shortcuts; verify on hardware.
- Plus/minus physical IDs are not documented in `docs/API.md`; inspect logs or existing constants before exposing symbolic events.
- Move track external receive channel may need to come from slot receive-channel state or Move settings; default to active track channel only after confirming.
- Key/scale initial parse requires locating the authoritative set file schema.
- Sentry key/scale breadcrumbs may be advisory; set-file parse should be the initial source of truth.
- Scoped LED ownership should extend `shadow_led_queue.c`; a separate LED implementation would duplicate fragile restore behavior.
