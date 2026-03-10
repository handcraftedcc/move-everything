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

echo "PASS: shadow midi_exec before wiring present"
