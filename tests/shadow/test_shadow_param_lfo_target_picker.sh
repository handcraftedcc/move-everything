#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if rg -q 'moduleData\.module === "param_lfo"' "$file" || rg -q 'moduleData\.module === "param_lfo_fx"' "$file"; then
  echo "FAIL: picker still uses module-specific gating" >&2
  exit 1
fi
if ! rg -q 'VIEWS\.DYNAMIC_PARAM_PICKER' "$file"; then
  echo "FAIL: dynamic picker view is missing" >&2
  exit 1
fi
if ! rg -q 'openDynamicParamPicker' "$file"; then
  echo "FAIL: dynamic picker entrypoint is missing" >&2
  exit 1
fi
if ! rg -q 'handleDynamicParamPickerSelect' "$file"; then
  echo "FAIL: dynamic picker select handler is missing" >&2
  exit 1
fi
if ! rg -q 'drawDynamicParamPicker' "$file"; then
  echo "FAIL: dynamic picker draw handler is missing" >&2
  exit 1
fi
if ! rg -q 'module_picker' "$file"; then
  echo "FAIL: missing module_picker metadata support" >&2
  exit 1
fi
if ! rg -q 'parameter_picker' "$file"; then
  echo "FAIL: missing parameter_picker metadata support" >&2
  exit 1
fi
if ! rg -q 'getDynamicPickerMeta' "$file"; then
  echo "FAIL: missing generic dynamic picker metadata resolver" >&2
  exit 1
fi
if ! rg -q 'buildModulePickerOptions' "$file"; then
  echo "FAIL: missing module picker option builder" >&2
  exit 1
fi
if ! rg -q 'buildParameterPickerOptions' "$file"; then
  echo "FAIL: missing parameter picker option builder" >&2
  exit 1
fi
if ! rg -q 'buildDynamicPickerTargetItems' "$file"; then
  echo "FAIL: dynamic picker target item builder missing" >&2
  exit 1
fi
if ! rg -q 'buildDynamicPickerParamItemsForTarget' "$file"; then
  echo "FAIL: dynamic picker parameter item builder missing" >&2
  exit 1
fi
if ! rg -q 'getKnobTargets\(' "$file"; then
  echo "FAIL: module picker does not source targets from loaded components" >&2
  exit 1
fi
if ! rg -q 'getNumericParamsForTarget\(slot, target, numericOnly' "$file"; then
  echo "FAIL: parameter picker is missing metadata-based param filtering" >&2
  exit 1
fi
if rg -q 'endsWith\("_target_param"\)' "$file" || rg -q 'endsWith\("_target_component"\)' "$file"; then
  echo "FAIL: picker still relies on naming-convention fallback" >&2
  exit 1
fi
if ! rg -q '\$\{childPrefix\}\$\{i\}_\$\{key\}' "$file"; then
  echo "FAIL: picker does not expand child-prefixed parameter keys" >&2
  exit 1
fi

echo "PASS: shadow dynamic picker UI + metadata integration markers are present"
