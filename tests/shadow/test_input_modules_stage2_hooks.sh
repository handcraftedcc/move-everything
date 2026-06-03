#!/usr/bin/env bash
set -euo pipefail

api_file="src/host/input_module_api_v1.h"
runtime_file="src/host/shadow_input_modules.c"
runtime_header="src/host/shadow_input_modules.h"
watcher_file="src/host/move_mode_watcher.c"
watcher_header="src/host/move_mode_watcher.h"
shim_file="src/schwung_shim.c"
chain_mgmt_file="src/host/shadow_chain_mgmt.c"
build_file="scripts/build.sh"
ui_input_file="src/shadow/shadow_ui_input_modules.mjs"
chromatic_meta="src/modules/inputs/true-chromatic-input/module.json"
chromatic_dsp="src/modules/inputs/true-chromatic-input/dsp/input_plugin.c"

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

for f in "$api_file" "$runtime_file" "$runtime_header" "$watcher_file" "$watcher_header" "$chromatic_meta" "$chromatic_dsp"; do
  require_file "$f"
done

require_rg "INPUT_MODULE_MAX_OUTPUT_PACKETS" "$api_file" "input API should bound module output packets"
require_rg "input_usb_midi_packet_t" "$api_file" "input API should define USB-MIDI packets"
require_rg "input_context_t" "$api_file" "input API should expose input context"
require_rg "input_process_result_t" "$api_file" "input API should expose process results"
require_rg "input_module_api_v1_t" "$api_file" "input API should define module v1 contract"
require_rg "host_input_api_v1_t" "$api_file" "input API should define host callbacks"
require_rg "schwung_input_module_init_v1" "$api_file" "input API should define module init symbol"

require_rg "shadow_input_set_track_module" "$runtime_file" "runtime should load selected track modules"
require_rg "shadow_input_process_pad_event" "$runtime_file" "runtime should process pad events"
require_rg "shadow_input_process_control_event" "$runtime_file" "runtime should observe non-blocked input control events"
require_rg "shadow_input_panic_all" "$runtime_file" "runtime should provide panic release"
require_rg "generated_notes\\[16\\]\\[128\\]" "$runtime_file" "runtime should track generated notes"
require_rg "uiOctaveIndex" "$runtime_file" "runtime should initialize track octaves from set uiOctaveIndex"
require_rg "CC_UP 55" "$runtime_file" "runtime should use symbolic up button constant"
require_rg "CC_DOWN 54" "$runtime_file" "runtime should use symbolic down button constant"
require_rg "p->cable != 2" "$runtime_file" "runtime should validate cable-2 output"
require_rg "shadow_chain_midi_inject" "$shim_file" "input runtime should reuse existing MIDI inject path"
require_rg "input_module:" "$shim_file" "shim should accept input_module control params"
require_rg "shadow_input_process_pad_event" "$shim_file" "shim should hook pad interception"
require_rg "shadow_input_process_control_event" "$shim_file" "shim should observe octave controls without filtering them"
require_rg "strncmp\\(key, \"input_module:\", 13\\)" "$chain_mgmt_file" "chain param dispatcher should route input_module params"

require_rg "move_mode_watcher_start" "$shim_file" "shim should start cached mode watcher"
require_rg "Set MainMode|mainmode" "$watcher_file" "mode watcher should parse MainMode breadcrumbs"

require_rg "input_module:module" "$ui_input_file" "UI should sync selected module to shim runtime"
require_rg "input_module:param:" "$ui_input_file" "UI should sync input params to shim runtime"
require_rg "input_module:state_dir" "$ui_input_file" "UI should sync set-state directory to shim runtime"

require_rg '"id"[[:space:]]*:[[:space:]]*"true-chromatic-input"' "$chromatic_meta" "true chromatic metadata should use expected id"
require_rg '"component_type"[[:space:]]*:[[:space:]]*"input_module"' "$chromatic_meta" "true chromatic should be an input module"
require_rg '"dsp"[[:space:]]*:[[:space:]]*"dsp.so"' "$chromatic_meta" "true chromatic should ship a DSP"
require_rg "schwung_input_module_init_v1" "$chromatic_dsp" "true chromatic should export input API init"
require_rg "PAD_FIRST 68" "$chromatic_dsp" "true chromatic should map Move pad note range"
require_rg "ctx->active_track" "$chromatic_dsp" "true chromatic should emit on active track channel"
require_rg "ctx->octave_index" "$chromatic_dsp" "true chromatic should follow host-provided track octave"
require_rg "root_note \\+ \\(\\(octave_index - 2\\) \\* 12\\) \\+ idx" "$chromatic_dsp" "true chromatic should map pads to one contiguous 32-note chromatic range from the current octave"
if rg -q "3 - row|row_interval" "$chromatic_dsp"; then
  echo "FAIL: true chromatic mapping should not use row intervals or inverted rows" >&2
  exit 1
fi
if rg -q '"row_interval"' "$chromatic_meta"; then
  echo "FAIL: true chromatic UI should not expose row spacing that can create overlaps" >&2
  exit 1
fi

require_rg "shadow_input_modules\\.c" "$build_file" "build should compile input runtime into shim"
require_rg "move_mode_watcher\\.c" "$build_file" "build should compile mode watcher into shim"
require_rg "true-chromatic-input/dsp/input_plugin\\.c" "$build_file" "build should compile true chromatic DSP"

echo "PASS: Stage 2 input module API, runtime, interception, and test module hooks are present"
