#!/usr/bin/env bash
set -euo pipefail

file="src/modules/midi_fx/param_lfo/dsp/param_lfo.c"

if ! rg -q '#define MOD_EMIT_EVERY_N_BLOCKS 2' "$file"; then
  echo "FAIL: param_lfo modulation emit throttle constant is missing or not set to 2 blocks" >&2
  exit 1
fi
if ! rg -q 'unsigned int block_tick_count;' "$file"; then
  echo "FAIL: param_lfo block tick counter state is missing" >&2
  exit 1
fi
if ! rg -q 'block_tick_count % MOD_EMIT_EVERY_N_BLOCKS' "$file"; then
  echo "FAIL: param_lfo modulation write throttle gate is missing" >&2
  exit 1
fi
if ! rg -q 'float phase_inc = \(inst->rate_hz \* waveform_rate_multiplier\(inst\)\)' "$file"; then
  echo "FAIL: param_lfo phase advance path is missing" >&2
  exit 1
fi

echo "PASS: param_lfo modulation writes are throttled to every 2 blocks"
