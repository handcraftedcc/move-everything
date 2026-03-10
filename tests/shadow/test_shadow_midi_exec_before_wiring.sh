#!/usr/bin/env bash
set -euo pipefail

ui="src/shadow/shadow_ui.js"
shim="src/move_anything_shim.c"
mgmt="src/host/shadow_chain_mgmt.c"
types="src/host/shadow_chain_types.h"
set_pages="src/host/shadow_set_pages.c"
state="src/host/shadow_state.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -n "midi_exec_before" "$types" >/dev/null 2>&1; then
  echo "FAIL: shadow slot type missing midi_exec_before field" >&2
  exit 1
fi

if ! rg -n "slot:midi_exec" "$ui" >/dev/null 2>&1; then
  echo "FAIL: shadow UI chain settings missing slot:midi_exec control" >&2
  exit 1
fi

if ! rg -n "Midi Exec" "$ui" >/dev/null 2>&1; then
  echo "FAIL: shadow UI must label chain setting as Midi Exec" >&2
  exit 1
fi

if ! rg -n "slot:midi_exec" "$mgmt" >/dev/null 2>&1; then
  echo "FAIL: shadow chain mgmt missing slot:midi_exec param set/get handling" >&2
  exit 1
fi

if ! rg -n "midi_exec" "$set_pages" >/dev/null 2>&1; then
  echo "FAIL: set-page config save/load must persist midi_exec" >&2
  exit 1
fi

if ! rg -n "slot_midi_exec" "$state" >/dev/null 2>&1; then
  echo "FAIL: shadow state save/load must persist slot_midi_exec values" >&2
  exit 1
fi

if ! rg -n "midi_send_external" "$mgmt" >/dev/null 2>&1; then
  echo "FAIL: chain host API in shadow chain mgmt must wire midi_send_external callback" >&2
  exit 1
fi

if ! rg -n "midi_exec_before" "$shim" >/dev/null 2>&1; then
  echo "FAIL: shim missing midi_exec_before routing logic" >&2
  exit 1
fi

route_ctx=$(sed -n '/static void shadow_route_midi_exec_before_from_midi_in/,/^}/p' "$shim")
if ! echo "$route_ctx" | rg -q "p2 < 68"; then
  echo "FAIL: midi_exec_before should ignore non-pad notes below 68" >&2
  exit 1
fi
if ! echo "$route_ctx" | rg -q "p2 > 99"; then
  echo "FAIL: midi_exec_before should ignore non-pad notes above 99" >&2
  exit 1
fi

rt_ctx=$(sed -n '/Handle system realtime messages/,/Done with this packet/p' "$shim")
if ! echo "$rt_ctx" | rg -q "slot->midi_exec_before"; then
  echo "FAIL: shim must forward internal realtime to midi_exec_before slots" >&2
  exit 1
fi
if ! echo "$rt_ctx" | rg -q "MOVE_MIDI_SOURCE_INTERNAL"; then
  echo "FAIL: internal realtime forward must use internal source tag" >&2
  exit 1
fi

echo "PASS: shadow midi_exec before wiring present"
