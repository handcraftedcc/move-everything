#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'function getSlotLfoStatePath\(slotIndex, stateDir\)' "$file"; then
  echo "FAIL: missing per-slot LFO state path helper" >&2
  exit 1
fi

if ! rg -q 'function saveSlotLfoStateToFile\(slotIndex\)' "$file"; then
  echo "FAIL: missing slot LFO save helper" >&2
  exit 1
fi

if ! rg -q 'saveSlotLfoStateToFile\(i\);' "$file"; then
  echo "FAIL: autosave path does not persist slot LFO state" >&2
  exit 1
fi

if ! rg -q 'restoreSlotLfosFromStateDir\(activeSlotStateDir\);' "$file"; then
  echo "FAIL: slot LFO state is not restored for active set" >&2
  exit 1
fi

if ! rg -q 'host_write_file\(getSlotLfoStatePath\(i, newDir\), "\[\]\\n"\);' "$file"; then
  echo "FAIL: new set initialization does not seed slot LFO state files" >&2
  exit 1
fi

if ! rg -q 'host_write_file\(getSlotLfoStatePath\(i, newDir\), slotLfos\);' "$file"; then
  echo "FAIL: duplicated set flow does not copy slot LFO state files" >&2
  exit 1
fi

echo "PASS: slot LFO per-set persistence hooks are present"
