#!/usr/bin/env bash
set -euo pipefail

shim="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Deterministic insertion policy: append after mailbox tail, then wrap.
if ! rg -n "last_non_empty_slot" "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing mailbox tail scan for deterministic insertion" >&2
  exit 1
fi

if ! rg -n "search_start = j \\+ 4" "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing contiguous-prefix append search logic" >&2
  exit 1
fi

# Duplicate-edge coalescing: avoid repeated note on/off transitions.
if ! rg -n "shadow_midi_to_move_is_duplicate_edge" "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing duplicate-edge coalescing helper" >&2
  exit 1
fi

# Diagnostic hook for mailbox occupancy and insertion position.
if ! rg -n "midi_to_move diag" "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing midi_to_move diagnostic telemetry log" >&2
  exit 1
fi

echo "PASS: MIDI-to-Move stability guards present"
