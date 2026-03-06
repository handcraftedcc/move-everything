#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'cachedKnobContextsComp = \(view === VIEWS\.HIERARCHY_EDITOR\) \? hierEditorComponent : selectedChainComponent;' "$file"; then
  echo "FAIL: knob context cache does not key hierarchy view by component" >&2
  exit 1
fi
if ! rg -q 'const currentComp = \(view === VIEWS\.HIERARCHY_EDITOR\) \? hierEditorComponent : selectedChainComponent;' "$file"; then
  echo "FAIL: knob context cache validation does not include hierarchy component" >&2
  exit 1
fi
if ! rg -q 'function enterHierarchyEditor\(slotIndex, componentKey\)' "$file" || ! rg -q 'invalidateKnobContextCache\(\);' "$file"; then
  echo "FAIL: hierarchy editor entry missing knob cache invalidation hook" >&2
  exit 1
fi

echo "PASS: hierarchy knob context cache keys by component and invalidates on entry"
