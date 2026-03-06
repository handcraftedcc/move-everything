#!/usr/bin/env bash
set -euo pipefail

meta="src/modules/midi_fx/param_lfo/module.json"

if ! rg -q '"knobs": \["rate_hz", "depth", "phase", "offset"\]' "$meta"; then
  echo "FAIL: param_lfo knob layout is missing rate/depth/phase/offset mapping" >&2
  exit 1
fi
if rg -q '"knobs": \[[^]]*"waveform"' "$meta"; then
  echo "FAIL: param_lfo knob layout still includes waveform" >&2
  exit 1
fi

echo "PASS: param_lfo knob layout excludes waveform and includes phase/offset"
