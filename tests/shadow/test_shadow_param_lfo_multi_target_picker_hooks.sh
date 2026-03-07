#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'function inferLinkedTargetComponentKey' "$file"; then
  echo "FAIL: missing generic linked target_key resolver" >&2
  exit 1
fi
if ! rg -q 'function inferLinkedTargetParamKey' "$file"; then
  echo "FAIL: missing generic linked param_key resolver" >&2
  exit 1
fi
if rg -q 'endsWith\("_target_param"\)' "$file" || rg -q 'endsWith\("_target_component"\)' "$file"; then
  echo "FAIL: linked resolver still depends on naming conventions" >&2
  exit 1
fi
if ! rg -q 'meta.target_key' "$file"; then
  echo "FAIL: linked target resolver does not support explicit target_key metadata" >&2
  exit 1
fi
if ! rg -q 'meta.param_key' "$file"; then
  echo "FAIL: linked param resolver does not support explicit param_key metadata" >&2
  exit 1
fi
if ! rg -q 'numeric_only' "$file"; then
  echo "FAIL: parameter picker is missing numeric_only metadata support" >&2
  exit 1
fi

echo "PASS: shadow dynamic picker link resolution is metadata-driven"
