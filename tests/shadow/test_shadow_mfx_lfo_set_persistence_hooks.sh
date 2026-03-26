#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'const MASTER_FX_LFO_STATE_FILE = "master_fx_lfos.json";' "$file"; then
  echo "FAIL: missing per-set Master FX LFO state file constant" >&2
  exit 1
fi

if ! rg -q 'function saveMasterFxLfoStateToFile\(' "$file"; then
  echo "FAIL: missing helper that writes Master FX LFO state to per-set file" >&2
  exit 1
fi

if ! rg -q 'host_write_file\(getMasterFxLfoStatePath\(\)' "$file"; then
  echo "FAIL: Master FX LFO state is not written to active per-set directory" >&2
  exit 1
fi

if ! rg -q 'function loadMasterFxLfoStateFromFile\(' "$file"; then
  echo "FAIL: missing helper that restores Master FX LFO state from per-set file" >&2
  exit 1
fi

if ! rg -q 'loadMasterFxLfoStateFromFile\(activeSlotStateDir, true\);' "$file"; then
  echo "FAIL: set-change reload path does not restore per-set Master FX LFO state" >&2
  exit 1
fi

if ! rg -q 'host_write_file\(newDir \+ "/" \+ MASTER_FX_LFO_STATE_FILE' "$file"; then
  echo "FAIL: new set initialization does not seed Master FX LFO state file" >&2
  exit 1
fi

echo "PASS: per-set Master FX LFO state save/load hooks are present"
