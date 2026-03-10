#!/usr/bin/env bash
set -euo pipefail

shim="src/move_anything_shim.c"
chain="src/modules/chain/dsp/chain_host.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -n "MIDI_FX_ACTIVE_WINDOW_MS" "$chain" >/dev/null 2>&1; then
  echo "FAIL: Missing MIDI_FX_ACTIVE_WINDOW_MS activity window constant" >&2
  exit 1
fi

if ! rg -n "midi_fx_last_activity_ms" "$chain" >/dev/null 2>&1; then
  echo "FAIL: Missing per-instance MIDI FX activity timestamp state" >&2
  exit 1
fi

if ! rg -n "midi_fx_active_recent\\(" "$chain" >/dev/null 2>&1; then
  echo "FAIL: Missing midi_fx_active_recent helper" >&2
  exit 1
fi

if ! rg -n "mark_midi_fx_activity\\(" "$chain" >/dev/null 2>&1; then
  echo "FAIL: Missing mark_midi_fx_activity helper" >&2
  exit 1
fi

start=$(rg -n "static int v2_get_param\\(" "$chain" | head -n 1 | cut -d: -f1 || true)
end=$(rg -n "^/\\* V2 render_block handler \\*/" "$chain" | head -n 1 | cut -d: -f1 || true)
if [ -z "${start}" ] || [ -z "${end}" ]; then
  echo "FAIL: could not locate v2_get_param boundaries in ${chain}" >&2
  exit 1
fi
ctx=$(sed -n "${start},${end}p" "$chain")
if ! echo "$ctx" | rg -q "\"midi_fx_active_recent\""; then
  echo "FAIL: chain v2 get_param must expose midi_fx_active_recent" >&2
  exit 1
fi

if ! rg -n "\"midi_fx_active_recent\"" "$shim" >/dev/null 2>&1; then
  echo "FAIL: shim idle gate must query midi_fx_active_recent from chain" >&2
  exit 1
fi

echo "PASS: MIDI FX idle activity window wiring present"
