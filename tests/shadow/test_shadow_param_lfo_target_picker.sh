#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'param_lfo' "$file"; then
  echo "FAIL: missing param_lfo integration marker" >&2
  exit 1
fi
if ! rg -q 'target_param' "$file"; then
  echo "FAIL: missing target_param handling" >&2
  exit 1
fi
if ! rg -q 'getKnobTargets\(' "$file"; then
  echo "FAIL: picker does not reuse getKnobTargets" >&2
  exit 1
fi
if ! rg -q 'VIEWS\.PARAM_LFO_TARGET_PICKER' "$file"; then
  echo "FAIL: missing dedicated PARAM_LFO_TARGET_PICKER view" >&2
  exit 1
fi
if ! rg -q 'getNumericParamsForTarget\(' "$file"; then
  echo "FAIL: missing numeric-only target param filter" >&2
  exit 1
fi
if ! rg -q 'setView\(VIEWS\.PARAM_LFO_TARGET_PICKER\)' "$file"; then
  echo "FAIL: picker entry does not use dedicated view" >&2
  exit 1
fi
if rg -q 'paramLfoTargetPickerActive' "$file"; then
  echo "FAIL: legacy knob picker reuse marker still present" >&2
  exit 1
fi

echo "PASS: shadow param_lfo target picker integration markers are present"
