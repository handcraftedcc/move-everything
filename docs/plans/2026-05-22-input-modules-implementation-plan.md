# Input Modules Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Date:** 2026-05-22

**Goal:** Build a track-scoped Input Module system that can optionally replace Move's native pad input with module-generated cable-2 MIDI while preserving native behavior by default.

**Architecture:** Input Modules are regular modules with `component_type: "input_module"` and a reserved `native` fallback. Shadow UI owns selection and persistence; the shim owns realtime gating, MIDI interception, panic release, and LED ownership. Expensive detection and file I/O run outside the SPI path.

**Tech Stack:** C shim/host code, QuickJS Shadow UI modules, existing module metadata, existing `/schwung-midi-inject` SHM, existing LED queue, existing per-set state directories.

---

## Stage 0: Confirm Integration Points

### Task 0.1: Verify Step 9 And Plus/Minus IDs

**Files:**
- Inspect: `src/shared/constants.mjs`
- Inspect: `src/schwung_shim.c`
- Optional docs update: `docs/plans/2026-05-22-input-modules-integration-map.md`

**Steps:**
1. Enable MIDI logging on device.
2. Press Step 9 with Shift+Volume Touch and confirm the note value.
3. Press plus/minus and record their CC/note IDs.
4. Update the integration map if observed values differ from the inferred values.

**Verify:**
Expected Step 9 is note `24` if Step 1 is note `16`.

### Task 0.2: Confirm Set Key/Scale Source

**Files:**
- Inspect: `/data/UserData/Sets/<uuid>/<name>/Song.abl` on device
- Inspect: `src/host/shadow_set_pages.c`
- Inspect: `src/host/shadow_dbus.c`

**Steps:**
1. Locate where root note and scale are stored in the set file.
2. Confirm Sentry/user-message text for runtime key/scale changes.
3. Record parse strategy in the integration map.

**Verify:**
Initial key/scale can be loaded without waiting for a user change breadcrumb.

## Stage 1: UI And Persistence Only

### Task 1.1: Add Input Module Metadata Support

**Files:**
- Modify: `src/host/module_manager.h`
- Modify: `src/host/module_manager.c`
- Modify: `src/shadow/shadow_ui.js`

**Steps:**
1. Extend module metadata if needed for `ui_input` and `input` fields.
2. Ensure `host_list_modules()` returns modules with `component_type: "input_module"`.
3. Keep existing module categories unchanged.

**Verify:**
Run `./scripts/build.sh`.

### Task 1.2: Add Native Input Module Metadata

**Files:**
- Create: `src/modules/inputs/input-native/module.json`

**Content:**
```json
{
  "id": "native",
  "name": "Native",
  "version": "1.0.0",
  "api_version": 2,
  "component_type": "input_module",
  "abbrev": "NAT",
  "description": "Use Move's native input unchanged."
}
```

**Verify:**
Build output includes the module metadata, and picker can still insert Native if the folder is missing.

### Task 1.3: Add Input Module Set State Helpers

**Files:**
- Modify: `src/shadow/shadow_ui.js`
- Create: `src/shadow/shadow_ui_input_modules.mjs`

**Steps:**
1. Read active set directory using existing set-change handling.
2. Load `input_modules.json`, defaulting missing tracks to `native`.
3. Save on selection/param changes with debouncing.
4. Never write from shim/SPI code.

**Verify:**
Manual: switch tracks, select different input modules, restart shadow UI, confirm selections persist.

### Task 1.4: Add Shortcut Flag And UI Entry

**Files:**
- Modify: `src/host/shadow_constants.h`
- Modify: `src/schwung_shim.c`
- Modify: `src/shadow/shadow_ui.js`

**Steps:**
1. Add `SHADOW_UI_FLAG_JUMP_TO_INPUT_MODULES`.
2. In the existing Shift+Volume+Step shortcut block, handle Step 9.
3. Launch shadow UI and set the flag.
4. In Shadow UI tick flag handling, enter the input module root for the selected track.

**Verify:**
Manual: `Shift + Volume Touch + Step 9` opens the input module picker.

### Task 1.5: Reuse Hierarchy Editor For Input Params

**Files:**
- Modify: `src/shadow/shadow_ui.js`
- Create/modify: `src/shadow/shadow_ui_input_modules.mjs`

**Steps:**
1. Add input param get/set shims.
2. Load declarative `ui_hierarchy` from module metadata first.
3. Append `Swap Input Module` to the root parameter list.
4. Route swap action back to the input picker.

**Verify:**
Manual: selected module params render with existing hierarchy behavior.

### Task 1.6: Add Param Lab Input Module

**Files:**
- Create: `src/modules/inputs/input-param-lab/module.json`

**Steps:**
1. Add compact `ui_hierarchy` covering supported param types.
2. Mark `component_type: "input_module"`.
3. Do not add DSP or MIDI interception behavior.

**Verify:**
Manual: edit each parameter type and confirm persistence by track and set.

## Stage 2: MIDI Interception

### Task 2.1: Add Input Module C API

**Files:**
- Create: `src/host/input_module_api_v1.h`

**Steps:**
1. Define `input_usb_midi_packet_t`.
2. Define `input_context_t`.
3. Define `input_process_result_t`.
4. Define `input_module_api_v1_t`.
5. Define `host_input_api_v1_t`.

**Verify:**
Run `./scripts/build.sh`.

### Task 2.2: Add Input Runtime Manager

**Files:**
- Create: `src/host/shadow_input_modules.c`
- Create: `src/host/shadow_input_modules.h`
- Modify: build scripts or Makefile section used by `scripts/build.sh`

**Steps:**
1. Maintain four track states.
2. Load/unload native `.so` modules outside the realtime path.
3. Expose param get/set for Shadow UI.
4. Track generated notes by channel/note.
5. Provide panic release.

**Verify:**
Build and load/unload a missing module without crashing.

### Task 2.3: Add Move Mode Watcher

**Files:**
- Create: `src/host/move_mode_watcher.c`
- Create: `src/host/move_mode_watcher.h`
- Modify: shim init/startup code

**Steps:**
1. Poll Sentry breadcrumb files every 100-250 ms.
2. Parse `Set MainMode`.
3. Publish cached mode and generation.
4. Leave existing `shadow_control->move_ui_mode` behavior intact until proven redundant.

**Verify:**
Manual: note/session/song overview changes update mode without SPI file I/O.

### Task 2.4: Hook Pad Interception

**Files:**
- Modify: `src/schwung_shim.c`
- Modify: `src/host/shadow_midi.c`
- Modify: `src/host/shadow_midi.h`
- Modify: `src/host/shadow_input_modules.c`

**Steps:**
1. In the MIDI_IN shadow copy/filter path, detect cable 0 pad note-on/off.
2. Check `mode == note`, active track module is non-native, and module is loaded.
3. Call `process_midi`.
4. Queue valid cable-2 outputs.
5. Zero original event only when `handled = 1`.
6. On error, pass through unchanged and schedule panic if needed.

**Verify:**
Native selection passes pads unchanged.

### Task 2.5: Add True Chromatic Input Module

**Files:**
- Create: `src/modules/inputs/true-chromatic-input/module.json`
- Create: `src/modules/inputs/true-chromatic-input/dsp/input_plugin.c`
- Modify: build packaging if needed

**Steps:**
1. Map pads 68-99 to chromatic notes.
2. Preserve note-off mapping for notes held before parameter changes.
3. Emit cable-2 note-on/off on active track channel.
4. Block original pad events.
5. Implement base note, row interval, layout, velocity mode, and follow scale params.

**Verify:**
Manual: pads play generated cable-2 notes in note mode only, with no stuck notes after track/mode/module changes.

## Stage 3: Pad LED Ownership

### Task 3.1: Add Pad-Only LED Owner

**Files:**
- Modify: `src/host/shadow_led_queue.c`
- Modify: `src/host/shadow_led_queue.h`
- Modify: `src/host/shadow_input_modules.c`

**Steps:**
1. Snapshot notes 68-99 on ownership entry.
2. Restore notes 68-99 on ownership exit.
3. Add `native` and `replace_pads`.
4. Gate ownership by note mode, active non-native module, and LED mode.

**Verify:**
Manual: non-pad LEDs continue to behave natively.

### Task 3.2: Add Module Pad LED Host API

**Files:**
- Modify: `src/host/input_module_api_v1.h`
- Modify: `src/host/shadow_input_modules.c`
- Modify: `src/host/shadow_led_queue.c`

**Steps:**
1. Implement `set_pad_led`.
2. Implement `get_pad_led`.
3. Implement `get_track_color` from active set `Song.abl` top-level
   `tracks[].color`.
4. Add redraw callback on mode/track/set changes.

**Verify:**
Manual: module can set all 32 pads, and leaving note mode restores native pads.

**Status 2026-05-22:** Implemented. The scoped owner is in
`shadow_led_queue.c`, `replace_pads` is read from input module metadata or the
persisted `led_mode` param, and True Chromatic redraws pad LEDs from the host
root/scale and set-file track color. Root/scale is seeded from active
`Song.abl` on set load, then fresh `/data/UserData/Sentry/*.run`
`__sentry-breadcrumb1/2` `Scale` and `Root note` set-to messages update the
same key/scale generation for live redraws.

## Stage 4: Documentation And Examples

### Task 4.1: Add Public Input Module Docs

**Files:**
- Create: `docs/INPUT_MODULES.md`
- Modify: `docs/MODULES.md`
- Modify: `docs/API.md`
- Modify: `docs/ARCHITECTURE.md`

**Steps:**
1. Document metadata.
2. Document C API.
3. Document JS UI helpers.
4. Document persistence.
5. Document realtime restrictions.
6. Document failure behavior.

**Verify:**
Docs match implemented APIs and filenames exactly.

### Task 4.2: Add Examples

**Files:**
- Create: `examples/input_modules/native/`
- Create: `examples/input_modules/true_chromatic/`
- Create: `examples/input_modules/param_lab/`

**Steps:**
1. Copy minimal metadata examples.
2. Include a small true chromatic source example.
3. Include a param lab hierarchy example.

**Verify:**
Examples build or are explicitly documentation-only.

## Final Verification

Run:

```bash
./scripts/build.sh
```

Install:

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

Manual acceptance:

1. Existing sets start with `native`.
2. `Shift + Volume Touch + Step 9` opens input module UI.
3. Param lab edits persist by track and set.
4. True Chromatic intercepts only in note mode.
5. Session and set overview pass through unchanged.
6. Track/module/mode/set transitions release held generated notes.
7. Replace pad LED mode restores native pad LEDs on exit.
