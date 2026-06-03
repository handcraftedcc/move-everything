# Input Modules Design

**Date:** 2026-05-22

**Requirements:** `docs/plans/2026-05-22-schwung_input_modules_developer_requirements.md`

## Goal

Add a track-scoped Input Module system that can intercept Move's physical pad input before the stock note engine, transform it in a selected module, and re-inject generated MIDI through cable 2 so Move plays its normal track instruments.

Default behavior must remain `native`: no interception, no LED ownership, no user-visible change for existing sets.

## Recommended Approach

Implement Input Modules as normal Schwung modules with `component_type: "input_module"` plus a reserved `native` module. This keeps discovery, packaging, metadata, and UI behavior aligned with existing modules.

Runtime behavior should live in the shim/host layer, not in JavaScript. JS should drive selection, parameters, and custom UI only. Pad transforms must be native and bounded because they run in the realtime MIDI/SPI path.

## Architecture

Input modules add one pre-native track-level layer:

```text
Move physical pad event
  -> shim MIDI_IN scan
  -> input runtime gate
  -> selected native input module process_midi()
  -> cable-2 MIDI injection queue
  -> Move external-MIDI track routing
```

The shim already owns the only safe place to filter Move's SPI MIDI_IN buffer. The existing `/schwung-midi-inject` queue already handles cable-2 injection, hardware-event deferral, and rate limiting, so input modules should call a thin wrapper over that path.

## State Model

State is per set and per track:

```text
/data/UserData/schwung/set_state/<set_uuid>/input_modules.json
```

Each track stores:

- selected module ID, default `native`
- parameter JSON
- LED mode, default `native`

Transient runtime state is never persisted: held notes, LED ownership, cached mode, transport, and key/scale generations stay in memory.

## UI Design

The shortcut is:

```text
Shift + Volume Touch + Step 9
```

The root view shows the active track's input module picker:

```text
Track 1 Input
  Native
  True Chromatic
  Chord Pads
```

After selection, the selected module uses the existing Shadow UI hierarchy editor. The only extra injected row is:

```text
Swap Input Module
```

This avoids a new parameter UI framework and preserves existing knob mapping, dynamic visibility, file browser, text entry, canvas, note, rate, and wav-position behavior.

## Realtime Safety

The SPI path may:

- read cached mode/track/key/transport values
- compare integers and module state
- call an already-loaded input module
- queue bounded MIDI output
- update a small held-note table
- queue LED updates

It must not scan files, parse JSON, allocate, log synchronously, execute JS, or take blocking locks.

## Failure Behavior

Every failure falls back to native behavior:

- missing module: select `native` for that track
- load failure: no pad blocking
- module error: no pad blocking and send panic note-offs
- invalid output: drop invalid packets
- mode leaves note: clear LED ownership and release generated notes

The selected module should never be able to strand Move in a blocked-input state.

## Staging

1. Integration map and code-level hook selection.
2. UI and persistence only.
3. MIDI interception and `true-chromatic-input`.
4. Pad LED ownership.
5. Public documentation and examples.

