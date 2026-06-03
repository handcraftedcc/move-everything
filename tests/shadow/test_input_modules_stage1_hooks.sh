#!/usr/bin/env bash
set -euo pipefail

shadow_file="src/shadow/shadow_ui.js"
input_file="src/shadow/shadow_ui_input_modules.mjs"
constants_file="src/host/shadow_constants.h"
shadow_c_file="src/shadow/shadow_ui.c"
shim_file="src/schwung_shim.c"
native_module="src/modules/inputs/input-native/module.json"
lab_module="src/modules/inputs/input-param-lab/module.json"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

require_file() {
  local file="$1"
  if [ ! -f "$file" ]; then
    echo "FAIL: missing $file" >&2
    exit 1
  fi
}

require_rg() {
  local pattern="$1"
  local file="$2"
  local message="$3"
  if ! rg -q "$pattern" "$file"; then
    echo "FAIL: $message" >&2
    exit 1
  fi
}

require_file "$input_file"
require_file "$native_module"
require_file "$lab_module"

require_rg "shadow_ui_input_modules\\.mjs" "$shadow_file" "shadow UI should import input module helpers"
require_rg "INPUT_MODULE_SELECT" "$shadow_file" "shadow UI should define an input module picker view"
require_rg "SHADOW_UI_FLAG_JUMP_TO_INPUT_MODULES" "$shadow_file" "shadow UI should consume the input-module shortcut flag"
require_rg "enterInputModuleSelect" "$shadow_file" "shadow UI should route into the input module picker"
require_rg "hierEditorComponent === \"input\"" "$shadow_file" "hierarchy editor should handle input-module return/swap paths"
require_rg "input:" "$shadow_file" "shadow UI should route input params through the shared hierarchy editor"

require_rg "input_modules\\.json" "$input_file" "input module state should persist to input_modules.json"
require_rg "function scanForInputModules" "$input_file" "input module helper should scan for input modules"
require_rg "function getInputSlotParam" "$input_file" "input module helper should expose param reads"
require_rg "function setInputSlotParam" "$input_file" "input module helper should expose param writes"
require_rg "function drawInputModuleSelect" "$input_file" "input module picker should be drawable"
require_rg "function handleInputModuleSelectSelect" "$input_file" "input module picker should apply selections"
require_rg "function hasFullInputModuleMetadata" "$input_file" "input module helper should distinguish skinny list metadata from full module.json metadata"
require_rg "cached && hasFullInputModuleMetadata\\(cached\\)" "$input_file" "input module metadata lookup should not return skinny host_list_modules cache entries"
require_rg "key\\.endsWith\\(\":base\"\\) \\|\\| key\\.endsWith\\(\":modulated\"\\)" "$input_file" "input module params should not synthesize chain-only :base/:modulated values"
require_rg "shadow_request_exit" "$input_file" "input module picker Back should exit Shadow UI instead of falling through to Shadow Chains"
require_rg "wasInputModule" "$shadow_file" "hierarchy Back should preserve input-module context"
require_rg "wasInputModule \\? \"Input Module\"" "$shadow_file" "input hierarchy Back should announce the input module picker, not Chain Editor"
require_rg "meta && meta\\.type === \"bool\"" "$shadow_file" "hierarchy display should format bool params explicitly"
require_rg "function toggleHierarchyBoolParam\\(key, fullKey\\)" "$shadow_file" "hierarchy bool params should share a click/jog toggle helper"
require_rg "const boolVal = parseMetaBool\\(currentVal\\)" "$shadow_file" "hierarchy jog editing should toggle bool params"
if rg -q "!hierEditorEditMode && meta && meta\\.type === \"bool\"" "$shadow_file"; then
  echo "FAIL: clicking a bool hierarchy param should enter edit mode, not toggle directly" >&2
  exit 1
fi
require_rg "beginHierarchyParamEdit\\(selectedKey\\)" "$shadow_file" "clicking a bool hierarchy param should use the generic edit-mode entry path"
require_rg "return \\{ label, value: \">\", key, isAction: true \\}" "$shadow_file" "hierarchy action rows should show right-side chevron affordance"

require_rg "#define SHADOW_UI_FLAG_JUMP_TO_INPUT_MODULES 0x100" "$constants_file" "input shortcut flag should use a non-colliding high bit"
require_rg "volatile uint16_t ui_flags" "$constants_file" "ui_flags should be wide enough for the high-bit input flag"
require_rg "uint16_t\\)mask" "$shadow_c_file" "shadow_clear_ui_flags should clear widened ui_flags safely"
require_rg "SHADOW_UI_FLAG_JUMP_TO_INPUT_MODULES" "$shim_file" "shim should set the input-module shortcut flag"
require_rg "d1 == 24" "$shim_file" "shim should bind Shift+Volume+Step 9 to note 24"

require_rg '"component_type"[[:space:]]*:[[:space:]]*"input_module"' "$native_module" "native metadata should be an input module"
require_rg '"id"[[:space:]]*:[[:space:]]*"native"' "$native_module" "native metadata should use reserved id native"
require_rg '"component_type"[[:space:]]*:[[:space:]]*"input_module"' "$lab_module" "param lab metadata should be an input module"
require_rg '"ui_hierarchy"' "$lab_module" "param lab should exercise hierarchy metadata"
require_rg '"level"[[:space:]]*:[[:space:]]*"advanced"' "$lab_module" "param lab should include a folder navigation item"
require_rg '"level"[[:space:]]*:[[:space:]]*"conditional"' "$lab_module" "param lab should include a conditional folder navigation item"
require_rg '"visible_if"[[:space:]]*:[[:space:]]*\{[[:space:]]*"param"[[:space:]]*:[[:space:]]*"show_conditional"' "$lab_module" "param lab should exercise conditional visibility"
require_rg '"key"[[:space:]]*:[[:space:]]*"show_conditional"' "$lab_module" "param lab should include a boolean visibility toggle"

echo "PASS: Stage 1 input module UI, persistence, and shortcut hooks are present"
