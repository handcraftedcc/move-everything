# Schwung Input Modules — Developer Requirements Document

## 0. Purpose

This document describes a staged implementation plan for a new **Input Module** system in Schwung.

The goal is to let Schwung intercept Move’s raw physical pad/key input while Move is in note-playing mode, run that input through a user-selected module, block the original native pad input when appropriate, and re-inject the module-generated MIDI through **MIDI cable 2** so Move sees it as external MIDI input.

This allows custom input modes to happen **before** Move’s native instruments and before the existing post-native MIDI-FX path.

Examples:

- true chromatic pad mode with no native overlaps
- custom drum layouts
- chord pads
- scale-aware layouts
- generative/walking input modes
- modules that optionally replace pad LEDs while active

The implementation must reuse Schwung’s existing module loading, Shadow UI, chain UI, persistence, MIDI injection, and LED restore systems wherever possible. Do **not** duplicate or mimic existing code unless reuse is impossible.

---

## 1. Core Problem

Currently, Move’s native OS handles physical pad/key input first. Schwung modules and MIDI-FX can operate after that, but this means:

1. Users are limited to Move’s native input modes.
2. MIDI-FX happen after Move’s own input processing and synth routing.
3. Custom pad layouts cannot fully replace native pad behavior.
4. Custom LED feedback is hard to keep synchronized with user-defined input modes.

The desired new path is:

```text
Physical Move pads/buttons
        ↓
Schwung input interception layer
        ↓
Selected Input Module
        ↓
Generated MIDI packets
        ↓
MIDI cable 2 injection
        ↓
Move sees result as external MIDI input
        ↓
Move track instrument plays normally
```

When no custom input module is active, Move should behave exactly as it does today.

---

## 2. High-Level Goals

### 2.1 Functional Goals

Implement a new module class/category:

```json
"component_type": "input_module"
```

or equivalent capability flag:

```json
"capabilities": {
  "input_module": true
}
```

Input modules must be discoverable from installed module folders and should use the same packaging model as existing Schwung modules.

Each input module should support:

- `module.json` metadata
- declarative Shadow UI parameter definitions
- optional custom UI JavaScript
- optional native DSP/shared-object implementation
- set/track-aware state persistence
- raw pad MIDI input callbacks
- output MIDI generation
- optional pad LED ownership
- access to active track, key, scale, plus/minus buttons, and transport state

### 2.2 UX Goals

The user opens the Input Module menu with:

```text
Shift + Volume Knob Touch + Step 9
```

The menu should feel like the existing chain UI:

```text
Input Module
  Native
  True Chromatic
  Chord Pads
  ...
```

After selecting a module, the user sees that module’s normal UI, using the same Shadow UI parameter system used by chain modules. A persistent **Swap** action should always appear at the bottom of the selected input module’s parameter list.

Default behavior:

- every track starts with `native`
- `native` means “do nothing; let Move handle input normally”
- the selected input module is tied to the active track and current set
- the selected input module and its parameter state are saved/restored with Schwung’s per-set/per-track state

### 2.3 Safety Goals

The custom input system must only intercept while all of these are true:

```text
current Move UI mode == note
active track has a non-native input module
input module is loaded and healthy
input interception is enabled for that module
```

When not true:

- physical pad input passes through unchanged
- no input transform runs
- custom pad LEDs are not written
- custom LED ownership is cleared
- Move’s native LEDs pass through normally
- any held module-generated notes are safely released

---

## 3. Existing Schwung Systems That Must Be Reused

### 3.1 Module Discovery and Packaging

Use the existing installed module folder structure:

```text
src/modules/<module-id>/
  module.json
  ui.js              optional
  ui_chain.js        optional precedent
  dsp.so             optional
  settings-schema.json optional
```

Input modules are grouped under the input-module category folder:

```text
src/modules/inputs/<module-id>/
  module.json
  ui.js              optional
  dsp.so             optional
```

Do not create a parallel module installer or parallel metadata parser.

Input modules should be normal modules with an added component type/capability. The core should only add the new plumbing required to load them in the input context.

Recommended `module.json` shape:

```json
{
  "id": "true-chromatic-input",
  "name": "True Chromatic Input",
  "version": "1.0.0",
  "api_version": 2,
  "component_type": "input_module",
  "abbrev": "CHR",
  "description": "Maps Move pads to a true chromatic layout and emits MIDI via cable 2.",
  "ui_hierarchy": {
    "levels": {
      "root": {
        "name": "True Chromatic",
        "params": [
          { "key": "root_note", "name": "Root", "type": "note", "mode": "multi", "min_note": 0, "max_note": 127 },
          { "key": "layout", "name": "Layout", "type": "enum", "options": ["4ths", "Chromatic Rows", "Drum Rack"] },
          { "key": "follow_set_scale", "name": "Follow Set Scale", "type": "bool" }
        ],
        "knobs": ["root_note", "layout", "follow_set_scale"]
      }
    }
  },
  "input": {
    "led_mode": "native",
    "emit_default_channel": "active_track"
  },
  "dsp": "dsp.so"
}
```

The existing loader has an 8 KB `module.json` limit, so keep UI definitions compact.

### 3.2 Shadow UI Parameter System

Input modules should reuse the existing Shadow UI hierarchy model:

- `ui_hierarchy` from `module.json`
- or dynamic `get_param("ui_hierarchy")`
- same supported parameter types
- same knob mapping logic
- same dynamic visibility logic
- same file browser/text/canvas mechanisms
- same parameter get/set path

Do not create an “input UI” framework that duplicates the chain parameter UI.

The input module screen should be a thin wrapper around the existing component parameter/hierarchy editor:

```text
[Input Module Name]
  Param 1
  Param 2
  Param 3
  ...
  Swap Input Module
```

### 3.3 Chain UI / Component UI Precedent

Signal Chain already has:

```text
Input or MIDI Source -> MIDI FX -> Sound Generator -> Audio FX -> Output
```

and patch JSON already models input routing, MIDI sources, components, and stored params. Input modules are not the same as chain MIDI sources, but the UI and parameter plumbing should be reused as much as possible.

Relevant pattern to inspect before coding:

```text
src/modules/chain/ui.js
src/shadow/shadow_ui.js
src/shadow/shadow_ui_patches.mjs
src/shadow/shadow_ui_slots.mjs
src/modules/chain/dsp/chain_host.c
```

Input modules should not be implemented as a hidden chain component unless that naturally falls out of the existing architecture. Conceptually, this is a **track-level pre-native input override**, not a MIDI source inside the audio/MIDI chain.

### 3.4 Existing MIDI Injection Path

Reuse the existing MIDI injection path that can write USB-MIDI packets into Move’s MIDI input mailbox.

Important behavior to preserve:

- the injector preserves the cable nibble chosen by the caller
- cable 2 is used for external USB/general MIDI routed to track instruments
- cable 0 is reserved for Move’s internal pad/button protocol
- injection is deferred when real hardware MIDI input is present to avoid racing Move’s firmware MIDI read path
- injection is rate-limited and carries remaining packets across ticks

Input modules should use this path or a thin wrapper around it.

Recommended helper:

```c
int shadow_input_emit_cable2(const uint8_t *packet4);
```

where `packet4` is a 4-byte USB-MIDI packet:

```c
packet4[0] = 0x20 | cin;   // cable 2 + CIN
packet4[1] = status;
packet4[2] = data1;
packet4[3] = data2;
```

For normal note-on on channel 1:

```c
{ 0x29, 0x90, note, velocity }
```

For note-off:

```c
{ 0x28, 0x80, note, release_velocity }
```

or note-on velocity 0 if desired:

```c
{ 0x29, 0x90, note, 0 }
```

### 3.5 Existing LED Queue / Overtake Restore Logic

Reuse and extend the existing LED queue concepts:

- Move LED state cache
- snapshot on ownership entry
- restore on ownership exit
- last-writer-wins pending LED queues
- rate-limited flush
- `skip_led_clear`
- per-CC passthrough behavior

Do **not** reimplement a separate LED restore system.

The input module LED system should be narrower than full overtake mode:

- only pad LEDs should be optionally owned by input modules
- default should be native pass-through
- track/buttons/transport LEDs should remain native unless explicitly added later
- restore must happen on mode change, track change, set change, module swap, or module unload

The relevant existing system already snapshots Move LED state, clears or preserves LEDs during overtake, blocks cable-0 LED packets while overtake owns LEDs, and restores from snapshots on exit. Input modules should either generalize that system to support scoped pad ownership or add a small scoped owner state to the same file.

### 3.6 Existing Per-Set State System

Reuse Schwung’s per-set/per-slot state system. Existing code already has:

- default slot state
- per-set state directories
- migration of default slot state to existing sets
- set-change detection
- a flag that pushes heavier set file I/O out of the audio/SPI path

Input modules should save in the same state root as other set-specific Schwung data.

Recommended file:

```text
/data/UserData/schwung/set_state/<set_uuid>/input_modules.json
```

Example:

```json
{
  "version": 1,
  "tracks": [
    {
      "track": 0,
      "module": "native",
      "params": {}
    },
    {
      "track": 1,
      "module": "true-chromatic-input",
      "params": {
        "layout": "4ths",
        "root_note": 48,
        "follow_set_scale": true
      }
    }
  ]
}
```

Do not do heavy file reads/writes from the realtime MIDI or audio/SPI path.

---

## 4. Required Runtime State

### 4.1 Move UI Mode State

Create a small watcher that tracks Move’s native UI mode from Sentry breadcrumb files:

```text
/data/UserData/Sentry/*.run/__sentry-breadcrumb1
/data/UserData/Sentry/*.run/__sentry-breadcrumb2
```

Relevant entries:

```text
Set MainMode (new state: note)
Set MainMode (new state: session)
Set MainMode (new state: songOverview)
```

Use:

```c
typedef enum {
    MOVE_MODE_UNKNOWN,
    MOVE_MODE_NOTE,
    MOVE_MODE_SESSION,
    MOVE_MODE_SET_OVERVIEW
} move_mode_t;

typedef struct {
    volatile move_mode_t mode;
    volatile uint32_t generation;
} move_mode_state_t;
```

Parsing:

```text
note         -> MOVE_MODE_NOTE
session      -> MOVE_MODE_SESSION
songOverview -> MOVE_MODE_SET_OVERVIEW
anything else -> MOVE_MODE_UNKNOWN
```

The watcher may poll at a modest interval. It must not run in the realtime MIDI path.

The realtime path should only read:

```c
state.mode
state.generation
```

Core gating rule:

```c
custom_input_allowed = (current_mode == MOVE_MODE_NOTE);
```

On generation change:

```c
if (state.generation != last_seen_generation) {
    last_seen_generation = state.generation;

    if (state.mode == MOVE_MODE_NOTE) {
        redraw_custom_input_leds();
    } else {
        clear_custom_led_ownership();
        stop_intercepting_pad_input();
        send_all_notes_off_for_input_module();
    }
}
```

### 4.2 Active Track State

Schwung already tracks active track. The input module runtime must use the existing active-track source and must not implement a duplicate track detector unless the existing one is insufficient.

The runtime needs:

```c
int active_track_index; // 0-3 or however Schwung currently represents tracks
uint32_t active_track_generation;
```

On active track change:

- unload or suspend previous track’s input module
- load/resume new track’s selected input module
- release held notes from previous module
- clear previous module’s LED ownership
- redraw new module’s pad LEDs if allowed
- update UI view if the Input Module menu is open

### 4.3 Active Set State

Input module selection and parameters must follow the currently loaded set.

On set load:

- load `/set_state/<set_uuid>/input_modules.json`
- initialize missing tracks to `native`
- if no per-set state exists, create defaults lazily on save
- never block realtime MIDI while reading or writing state

On set save / autosave:

- persist module IDs and parameter values per track
- do not persist transient runtime state like held notes, LED ownership, cached mode, or transport phase

### 4.4 Set Key / Scale State

Modules need simple access to the current set key and scale.

Initial state should be parsed from the loaded set file because Sentry only emits key/scale entries when they change.

Runtime updates should also listen for Sentry/user messages such as:

```text
Root note
set to A

Scale
set to Hirajoshi
```

Requirements:

- parse key/scale from set file on set load
- update from Sentry/user-message changes when observed
- keep a generation counter for key/scale changes
- expose a simple API to modules:

```c
typedef struct {
    int root_midi_class;       // C=0, C#=1, ... B=11, or -1 unknown
    char root_name[8];         // "A", "C#", etc.
    char scale_name[64];       // "Hirajoshi", etc.
    uint32_t generation;
} move_key_scale_state_t;
```

Module-facing helpers:

```c
int host_input_get_root_note_class(void *ctx);
const char* host_input_get_root_name(void *ctx);
const char* host_input_get_scale_name(void *ctx);
uint32_t host_input_get_key_scale_generation(void *ctx);
```

If key/scale is unknown, modules must receive an explicit unknown state, not stale data.

### 4.5 Plus/Minus Button State

Expose plus/minus activity to input modules.

The exact physical event IDs should be confirmed against existing constants and Move hardware mappings. The implementation must not hardcode unexplained magic numbers inside modules. Core should provide symbolic events.

Recommended enum:

```c
typedef enum {
    INPUT_BUTTON_PLUS,
    INPUT_BUTTON_MINUS
} input_button_t;

typedef enum {
    INPUT_BUTTON_DOWN,
    INPUT_BUTTON_UP,
    INPUT_BUTTON_REPEAT
} input_button_event_t;
```

Module callback:

```c
void on_input_button(void *instance,
                     input_button_t button,
                     input_button_event_t event,
                     const input_context_t *ctx);
```

Typical use cases:

- octave up/down
- root offset changes
- layout page changes
- scale degree offset

The core only reports the events. Modules decide what they mean.

Stage 2 implementation note:

- True Chromatic follows Move's native track octave by initializing from each
  track's `uiOctaveIndex` in `Song.abl` and observing Up/Down button presses
  without blocking those buttons from Move.

### 4.6 Native Transport State

Modules may want access to Move transport.

Expose cached, non-blocking values:

```c
typedef struct {
    int playing;
    double bpm;
    uint64_t tick_counter;
    uint32_t generation;
} input_transport_state_t;
```

Potential callbacks:

```c
void on_transport_changed(void *instance, const input_transport_state_t *state);
void on_transport_tick(void *instance, const input_transport_state_t *state);
```

For stage 1/2, it is acceptable to expose only coarse transport changes if full timing is not yet available.

---

## 5. Input Module API

### 5.1 Design Principle

Existing `plugin_api_v2_t` has `on_midi`, `set_param`, `get_param`, and `render_block`, but `on_midi` returns `void`. Input modules need to tell the host whether the original hardware event should be blocked and may emit zero or more replacement MIDI packets.

Therefore implement either:

1. a new `input_module_api_v1_t`, or
2. an optional extension struct queried from the existing plugin

Recommended: add a dedicated input API so the contract is clear and documented.

### 5.2 Proposed C API

Header:

```text
src/host/input_module_api_v1.h
```

Types:

```c
#define INPUT_MODULE_MAX_OUTPUT_PACKETS 16

typedef enum {
    INPUT_EVENT_PAD,
    INPUT_EVENT_BUTTON,
    INPUT_EVENT_TRANSPORT,
    INPUT_EVENT_MODE_CHANGED,
    INPUT_EVENT_TRACK_CHANGED,
    INPUT_EVENT_SET_CHANGED,
    INPUT_EVENT_KEY_SCALE_CHANGED
} input_event_type_t;

typedef struct {
    uint8_t cin;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t cable;
} input_usb_midi_packet_t;

typedef struct {
    int active_track;
    int move_mode;
    int root_note_class;
    const char *root_name;
    const char *scale_name;
    int playing;
    double bpm;
    uint32_t mode_generation;
    uint32_t track_generation;
    uint32_t key_scale_generation;
} input_context_t;

typedef struct {
    int handled;     // 1 = block original hardware event
    int output_count;
    input_usb_midi_packet_t outputs[INPUT_MODULE_MAX_OUTPUT_PACKETS];
    int request_led_redraw;
} input_process_result_t;
```

API:

```c
typedef struct input_module_api_v1 {
    int api_version;

    void* (*create_instance)(const char *module_dir,
                             const char *json_defaults);

    void (*destroy_instance)(void *instance);

    void (*set_param)(void *instance,
                      const char *key,
                      const char *value);

    int (*get_param)(void *instance,
                     const char *key,
                     char *buf,
                     int buf_len);

    int (*process_midi)(void *instance,
                        const input_usb_midi_packet_t *in,
                        const input_context_t *ctx,
                        input_process_result_t *out);

    void (*on_context_changed)(void *instance,
                               const input_context_t *ctx);

    void (*on_all_notes_off)(void *instance,
                             const input_context_t *ctx);
} input_module_api_v1_t;
```

Export:

```c
input_module_api_v1_t* schwung_input_module_init_v1(const host_input_api_v1_t *host);
```

### 5.3 Host API Provided to Input Modules

```c
typedef struct host_input_api_v1 {
    void *ctx;

    int (*emit_midi)(void *ctx,
                     const input_usb_midi_packet_t *packets,
                     int count);

    int (*set_pad_led)(void *ctx,
                       int pad_index,
                       uint8_t color);

    int (*get_pad_led)(void *ctx,
                       int pad_index);

    int (*get_track_color)(void *ctx,
                           int track_index);

    int (*get_active_track)(void *ctx);

    int (*get_root_note_class)(void *ctx);
    const char* (*get_root_name)(void *ctx);
    const char* (*get_scale_name)(void *ctx);

    int (*get_transport_playing)(void *ctx);
    double (*get_transport_bpm)(void *ctx);

    void (*log)(void *ctx, const char *message);
} host_input_api_v1_t;
```

### 5.4 Output MIDI Rules

Input modules should normally emit cable-2 packets.

Core should validate module outputs before injecting:

- valid cable is 2 for normal generated MIDI
- valid CIN/status pairing
- data bytes are 0–127
- max packets per event is bounded
- malformed output is dropped and logged
- module failure must not crash Schwung or Move

Modules may request channel behavior:

```json
"input": {
  "emit_default_channel": "active_track"
}
```

Possible values:

```text
active_track
preserve
1..16
```

Core should convert `active_track` to the correct channel for Move’s external MIDI routing.

---

## 6. MIDI Interception Behavior

### 6.1 Events to Intercept

Stage 2 should intercept only physical pad note events.

Likely Move pad notes:

```text
68–99
```

Use existing constants if available rather than duplicating numbers across files.

Initial intercept filter:

```c
is_pad_event =
    cable == 0 &&
    (status_type == 0x90 || status_type == 0x80) &&
    data1 >= MOVE_PAD_NOTE_FIRST &&
    data1 <= MOVE_PAD_NOTE_LAST;
```

Later, this can expand to other hardware controls if needed.

### 6.2 Gating

Only intercept when:

```c
current_mode == MOVE_MODE_NOTE
active_track_input_module != native
module_loaded == true
```

If false:

```text
pass through unchanged
do not call module
do not write custom LEDs
```

### 6.3 Handling / Blocking Rule

For each raw pad event:

1. Build an `input_usb_midi_packet_t`.
2. Call selected module’s `process_midi`.
3. If module returns `handled = 1`, block the original raw hardware event from reaching Move.
4. Inject replacement packets via cable 2.
5. If module returns `handled = 0`, pass original event through unchanged.
6. If module errors or times out, pass original event through and log once.

### 6.4 Held Notes / Panic

Core must maintain a minimal held-note tracker for module-generated notes so transitions do not leave stuck notes.

On any of these transitions:

- mode leaves note
- active track changes
- set changes
- selected module changes
- module unloads
- module crashes/errors
- Schwung exits/reloads

Core must send note-offs or All Notes Off for any notes generated by the input module.

Recommended:

- track generated notes by channel and note
- send note-off for each active generated note
- optionally also send CC 123 All Notes Off on the target channel as a fallback

### 6.5 Failure Behavior

Input interception must fail safe.

If input module loading fails:

```text
track falls back to native
no pad blocking
native Move behavior continues
log error
show UI warning if Input Module menu is open
```

If module output is invalid:

```text
drop invalid packets
keep native event blocked only if module explicitly handled and produced valid replacement
if no valid replacement and event was a note-off, still make sure panic path runs
```

---

## 7. UI Requirements

### 7.1 Entry Shortcut

Add:

```text
Shift + Volume Knob Touch + Step 9
```

This should open the Input Module screen for the currently active track.

Use existing shortcut/flag patterns. Do not create a separate polling path if existing Shadow UI shortcut dispatch can be extended.

### 7.2 Screens

#### Screen A — Input Module Root

For the active track:

```text
Track 1 Input
  Native
  True Chromatic
  Chord Pads
  Drum 32
```

Selecting a module:

- sets it for the active track
- loads or activates it
- opens its parameter UI

#### Screen B — Selected Module Parameter UI

Example:

```text
True Chromatic
  Layout: 4ths
  Root: C2
  Follow Set Scale: On
  LED Mode: Native
  Swap Input Module
```

The final item must always be the swap action.

#### Screen C — Swap Picker

Equivalent to root list, but initiated from inside the selected module UI.

### 7.3 UI Plumbing

Reuse existing:

- module scan/install metadata
- Shadow UI hierarchy renderer
- parameter editor
- knob assignment behavior
- dynamic visibility
- file browser
- text entry
- custom JS loading path where possible

Add only thin context glue:

```text
selected input module for active track
input module parameter namespace
swap action injection
```

### 7.4 Custom JS UI Support

Support custom JS UI for input modules.

Recommended metadata:

```json
"ui_input": "ui_input.js"
```

Fallback order:

1. `ui_input`
2. `ui_chain` if appropriate and compatible
3. `ui`
4. declarative `ui_hierarchy`

Custom input UI should receive callbacks similar to existing module UI plus input-specific host helpers:

```javascript
host_input_get_active_track()
host_input_get_key()
host_input_get_scale()
host_input_set_param(key, value)
host_input_get_param(key)
host_input_swap_module()
host_input_request_led_redraw()
```

Avoid adding JS-only realtime pad processing in the initial implementation. Realtime input transforms should live in native code for deterministic timing.

### 7.5 Param-Lab Input Test Module

Stage 1 should include a test module:

```text
src/modules/inputs/input-param-lab/
```

It should exercise all UI parameter types/features relevant to input modules:

- int
- float
- enum
- bool
- note
- rate
- filepath
- string
- wav_position
- canvas
- visible_if
- knob mapping
- dynamic params if supported

This module can be metadata/UI-only and does not need to intercept MIDI.

---

## 8. Persistence Requirements

### 8.1 Per-Set / Per-Track Storage

Persist selected input module and params by set and track.

Recommended:

```text
/data/UserData/schwung/set_state/<set_uuid>/input_modules.json
```

Must include:

- schema version
- one entry per track
- selected module ID
- parameter values
- optional LED mode setting
- optional module-specific state if needed

Do not include:

- held notes
- active pad LED colors unless module explicitly needs saved LED presets
- cached current Move mode
- cached transport state
- Sentry-derived key/scale cache unless needed only as a fast startup hint

### 8.2 Save Triggers

Save when:

- module is swapped
- parameter changes
- set save/autosave event occurs
- UI exits after dirty state
- Schwung shutdown if possible

Use existing dirty/autosave patterns. Do not write on every knob tick if the existing UI already debounces or batches parameter changes.

### 8.3 Load Triggers

Load when:

- Schwung starts
- set changes
- active track changes and the track’s module state is not loaded
- missing state should initialize to `native`

### 8.4 Migration

On first run after feature install:

- existing sets should default to all tracks `native`
- no behavior change for existing users
- no module selection should be copied between sets unless the existing per-set migration system explicitly copies default state

---

## 9. LED Requirements

### 9.1 Stage 3 Scope

LED support should be implemented after UI and MIDI interception are working.

Initial LED ownership is pad-only.

Pad LED notes:

```text
68–99
```

Do not block or rewrite:

- track buttons
- step buttons
- transport
- shift/menu/back
- knob LEDs

unless a future requirement explicitly expands scope.

### 9.2 LED Modes

Add per-module or per-track LED mode:

```json
"input": {
  "led_mode": "native"
}
```

Valid modes:

```text
native        pass Move LEDs through unchanged
replace_pads  block native pad LEDs and let module own pad LEDs
overlay_pads  allow native pad LEDs but let module overwrite selected pads
```

Stage 3 can implement only:

```text
native
replace_pads
```

and leave `overlay_pads` for later.

### 9.3 Ownership Gating

Custom pad LED ownership is active only when:

```text
current_mode == MOVE_MODE_NOTE
active track selected module != native
module led mode == replace_pads
```

When active:

- cache current Move pad LED state
- block cable-0 native pad LED packets for notes 68–99
- allow module pad LED writes
- do not block non-pad LEDs

When inactive:

- clear module LED ownership
- restore pad LEDs
- pass native LEDs normally

### 9.4 Transitions That Must Restore LEDs

Restore/clear ownership when:

- mode leaves note
- active track changes
- set changes
- selected input module changes
- module LED mode changes
- module unloads
- Schwung exits
- native module is selected
- module error/fallback occurs

### 9.5 Track Color API

Modules need easy access to the active track color.

Implement:

```c
int host_input_get_track_color(void *ctx, int track_index);
```

Implementation:

- read top-level `tracks[].color` from the active set `Song.abl`
- return an integer color/palette value
- return `-1` if unknown

Modules should not directly inspect LED cache internals.

---

## 10. State Detection Requirements

### 10.1 Sentry Watcher

Implement:

```text
src/host/move_mode_watcher.c
src/host/move_mode_watcher.h
```

Responsibilities:

- find newest Sentry run directory/file
- read breadcrumb files
- scan latest `Set MainMode`
- parse mode
- publish cached state and generation
- optionally scan key/scale user messages
- never run in realtime path

Polling interval:

```text
100–250 ms is fine initially
```

Avoid excessive file reads. Cache last file path, mtime, and size where possible.

### 10.2 Realtime Path

Realtime path should only read cached values.

Allowed:

```c
move_mode_t mode = g_move_mode_state.mode;
uint32_t gen = g_move_mode_state.generation;
```

Not allowed in realtime path:

- filesystem scans
- opening Sentry files
- parsing JSON/set files
- malloc/free
- blocking locks
- long string parsing

---

## 11. Staged Implementation Plan

## Stage 0 — Research and Integration Map

Before coding, inspect existing files and write a short implementation map.

Must inspect at minimum:

```text
docs/MODULES.md
docs/API.md
docs/ARCHITECTURE.md

src/shadow/shadow_ui.js
src/shadow/shadow_ui_patches.mjs
src/shadow/shadow_ui_slots.mjs
src/shadow/shadow_ui_ctx.mjs
src/shadow/shadow_ui_settings.mjs

src/modules/chain/ui.js
src/modules/chain/README.md
src/modules/chain/dsp/chain_host.c

src/host/module_manager.c
src/host/shadow_chain_mgmt.c
src/host/shadow_midi.c
src/host/shadow_led_queue.c
src/host/shadow_set_pages.c
src/host/shadow_state.c
src/schwung_shim.c
```

Deliverable:

```text
docs/plans/input-modules-integration-map.md
```

This should list:

- which functions will be reused
- which structs will be extended
- where the new shortcut belongs
- where input module state will be saved
- where MIDI interception will happen
- where LED ownership will hook in
- risks and unknowns

No behavior changes in Stage 0 except maybe docs.

---

## Stage 1 — UI and Persistence Only

Implement:

- module discovery for `component_type: input_module`
- input module picker
- shortcut Shift + Volume Touch + Step 9
- active-track-specific selection
- per-set/per-track save/load
- default `native`
- selected module parameter UI
- `Swap Input Module` item at bottom
- `input-param-lab` test module

Do **not** intercept MIDI yet.

Do **not** block or write LEDs yet.

Acceptance tests:

1. Open menu with shortcut.
2. See Native and installed input modules.
3. Select test module.
4. Edit params.
5. Switch tracks and see different input module assignments.
6. Save/reload set and confirm assignments persist.
7. Load a different set and confirm different assignments.
8. Select Native and confirm no runtime behavior changes.
9. Confirm declarative module UI uses existing Shadow UI hierarchy/param editor.

---

## Stage 2 — MIDI Interception and True Chromatic Module

Implement:

- Sentry mode watcher
- gating to note mode only
- pad note interception
- selected module native DSP loading
- input module `process_midi`
- output injection via cable 2
- blocking original pad input only when handled
- held-note tracker / panic
- plus/minus event callback
- key/scale read API
- transport read API if readily available
- `true-chromatic-input` test module

Do **not** implement custom LEDs yet.

### True Chromatic Test Module

Folder:

```text
src/modules/inputs/true-chromatic-input/
```

Required files:

```text
module.json
dsp/input_plugin.c
dsp.so
```

Possible params:

```json
{
  "layout": "chromatic_rows",
  "base_note": 48,
  "row_interval": 5,
  "velocity_mode": "pass",
  "channel_mode": "active_track",
  "follow_set_scale": false
}
```

Behavior:

- receives pad notes 68–99
- maps pads to a chromatic layout
- emits note-on/off via cable 2
- blocks original pad notes
- no LED handling
- plus/minus can shift octave or base note
- all generated note-offs match original generated note-ons even if params change while notes are held

Acceptance tests:

1. Native mode passes through unchanged.
2. Custom module only works in note mode.
3. Session/songOverview modes pass through unchanged.
4. Pad presses produce cable-2 note output.
5. Native pad note is blocked when module handles it.
6. Track switching changes target module/state.
7. Set switching loads correct module/state.
8. Held notes are released on module/mode/track changes.
9. No stuck notes after rapid switching.
10. Invalid/missing module falls back to native.

---

## Stage 3 — Pad LED Ownership

Implement:

- pad-only LED ownership
- `native` and `replace_pads` LED modes
- module pad LED API
- set-file track color API
- LED restore on all transitions
- module redraw callback on mode/track/set change
- LED panic/clear path on module error

Do not block non-pad LEDs.

Acceptance tests:

1. Native LED mode leaves Move LEDs untouched.
2. Replace mode blocks native pad LED packets only while in note mode.
3. Step/track/transport LEDs continue to work.
4. Module can set all 32 pad LEDs.
5. Track switch restores previous track/native pad LEDs.
6. Leaving note mode restores native LEDs.
7. Selecting Native restores native LEDs.
8. Module crash/unload restores native LEDs.
9. Rapid note/session toggling does not leave stale custom pad LEDs.

Implementation notes (2026-05-22):

- Scoped input pad ownership lives in `src/host/shadow_led_queue.c` beside the
  existing overtake restore logic.
- `replace_pads` snapshots/restores pad notes 68-99 only and blocks native
  cable-0 pad LED packets only while note mode/input ownership is active.
- True Chromatic defaults to `replace_pads` and colors root notes with the
  active track color parsed from `Song.abl`, in-scale notes white, and
  off-scale notes off.
- Initial root/scale state is parsed from the active set `Song.abl` and exposed
  through the input module API context/callbacks.
- Live root/scale changes are applied from fresh Sentry breadcrumb line pairs
  under `/data/UserData/Sentry/*.run/__sentry-breadcrumb{1,2}`, using native
  messages like `Scale` / `set to Harmonic Minor` and `Root note` /
  `set to C#`. Existing breadcrumb content is treated as historical; the set
  file remains the scene-load source of truth.

---

## Stage 4 — Documentation and Public API Finalization

Update:

```text
docs/MODULES.md
docs/API.md
docs/ARCHITECTURE.md
docs/INPUT_MODULES.md
```

Add examples:

```text
examples/input_modules/native/
examples/input_modules/true_chromatic/
examples/input_modules/param_lab/
```

Document:

- `component_type: input_module`
- `input` metadata fields
- C API
- JS UI API
- pad event format
- cable-2 output rules
- LED modes
- key/scale API
- plus/minus API
- transport API
- persistence behavior
- realtime safety rules
- module lifecycle
- failure/fallback behavior

---

## 12. Native Module

The default module must be:

```text
native
```

Meaning:

- no MIDI interception
- no LED ownership
- no custom UI required beyond display name
- Move behaves exactly as stock
- no extra overhead beyond checking the selected module ID

Implementation options:

### Option A — Pseudo Module

Core always inserts `native` at the top of the picker. It is not an installed module.

Pros:

- simple
- cannot be accidentally deleted
- clear fallback

Cons:

- slightly special-cased

### Option B — Real Metadata Module

Ship:

```text
src/modules/inputs/input-native/module.json
```

with no DSP and a reserved ID:

```json
{
  "id": "native",
  "name": "Native",
  "version": "1.0.0",
  "api_version": 2,
  "component_type": "input_module"
}
```

Core treats `native` as pass-through.

Pros:

- appears like other modules
- easier to document

Cons:

- still needs reserved behavior in core

Recommended: Option B for UI consistency, with core fallback behavior if the module folder is missing.

---

## 13. Realtime Safety Rules

The realtime MIDI/SPI path must not do:

- filesystem reads
- Sentry scans
- set file parsing
- JSON parsing
- malloc/free
- blocking locks
- unbounded loops over installed modules
- JS execution
- logging spam

Allowed in realtime path:

- read cached mode/track/key/transport state
- simple enum/integer comparisons
- call already-loaded native input module
- write bounded output packet arrays
- queue MIDI injection packets
- queue LED updates
- update small held-note arrays

All expensive work should happen in UI thread, watcher thread, or setup/load paths.

---

## 14. Suggested Internal Data Structures

```c
#define SCHWUNG_MAX_TRACKS 4
#define INPUT_MODULE_ID_MAX 64
#define INPUT_PARAM_JSON_MAX 4096

typedef struct {
    char module_id[INPUT_MODULE_ID_MAX];   // "native" by default
    char params_json[INPUT_PARAM_JSON_MAX];
    int led_mode;
    void *module_instance;
    input_module_api_v1_t *api;
    int loaded;
    int dirty;
} input_track_state_t;

typedef struct {
    input_track_state_t tracks[SCHWUNG_MAX_TRACKS];
    int active_track;
    uint32_t active_track_generation;
    uint32_t set_generation;
} input_runtime_state_t;
```

Held notes:

```c
static uint8_t generated_note_held[16][128];
```

LED ownership:

```c
typedef struct {
    int active;
    int track;
    char module_id[INPUT_MODULE_ID_MAX];
    uint32_t owner_generation;
} input_pad_led_owner_t;
```

---

## 15. Minimal Module Author Contract

An input module author should be able to think in terms of:

```c
process incoming pad event
return handled/not handled
emit replacement MIDI notes
optionally update pad LEDs
```

They should not need to understand:

- Sentry files
- set UUID detection
- Move breadcrumb parsing
- MIDI mailbox offsets
- LED restore snapshots
- active track internals
- UI routing internals

Those are core responsibilities.

---

## 16. Open Questions / Items to Verify During Stage 0

1. Exact physical event IDs for plus/minus buttons.
2. Whether active track’s external MIDI receive channel is directly available or must be inferred.
3. Best place to hook raw pad interception before Move consumes the event.
4. Whether existing chain MIDI injection can be called directly from the input module path or needs a named wrapper.
5. Whether `ui_chain.js` can be reused for custom input UI or if `ui_input.js` is cleaner.
6. Whether scoped pad LED ownership should live inside `shadow_led_queue.c` or a new file that calls into it.
7. Best source for transport state and tempo.
8. Exact set file location/schema for parsing root note and scale on load.
9. Whether Sentry key/scale messages are reliable enough for realtime updates or should be considered advisory.
10. Whether input modules should support `raw_midi` capability or always receive raw pad packets by definition.

---

## 17. Definition of Done

The feature is done when:

- Every track in every set can independently select an input module.
- Default behavior is `native` and indistinguishable from current Move behavior.
- Input module UI reuses Shadow UI parameter plumbing.
- Module selection and params save/load with sets.
- MIDI interception happens only in note mode.
- The true chromatic test module can block native pad notes and emit replacement MIDI through cable 2.
- Leaving note mode, changing track, changing module, or changing set cannot leave stuck notes.
- Optional pad LED replacement works and restores native LEDs reliably.
- Modules can read active key/scale, active track, plus/minus events, and transport state.
- All public APIs are documented.
- No duplicate UI framework or duplicate LED restore system was added.
