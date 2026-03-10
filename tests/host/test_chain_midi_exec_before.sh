#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -n "midi_exec_before" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing per-instance midi_exec_before state in chain host" >&2
  exit 1
fi

if ! rg -n "json_get_string\(json, \"midi_exec\"" "$file" >/dev/null 2>&1; then
  echo "FAIL: Patch parser must read midi_exec field" >&2
  exit 1
fi

if ! rg -n "inst->midi_exec_before =" "$file" >/dev/null 2>&1; then
  echo "FAIL: Patch load must apply midi_exec_before to instance state" >&2
  exit 1
fi

if ! rg -n "strcmp\(key, \"patch:midi_exec\"\)" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing patch:midi_exec set/get param plumbing" >&2
  exit 1
fi

start=$(rg -n "static void v2_on_midi\(void \*instance, const uint8_t \*msg, int len, int source\)" "$file" | head -n 1 | cut -d: -f1 || true)
end=$(rg -n "^/\* V2 set_param handler \*/" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${start}" ] || [ -z "${end}" ]; then
  echo "FAIL: could not locate v2_on_midi boundaries in ${file}" >&2
  exit 1
fi
ctx=$(sed -n "${start},${end}p" "$file")

if ! echo "$ctx" | rg -q "inst->midi_exec_before"; then
  echo "FAIL: v2_on_midi missing midi_exec_before before-mode branch" >&2
  exit 1
fi

if ! echo "$ctx" | rg -q "MOVE_MIDI_SOURCE_INTERNAL"; then
  echo "FAIL: before-mode branch must be scoped to internal source" >&2
  exit 1
fi

if ! echo "$ctx" | rg -q "midi_send_external"; then
  echo "FAIL: before-mode branch must emit processed MIDI to host external path" >&2
  exit 1
fi

echo "PASS: chain midi_exec before-mode wiring present"
