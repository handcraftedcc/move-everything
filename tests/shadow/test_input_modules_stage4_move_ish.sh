#!/usr/bin/env bash
set -euo pipefail

module_meta="src/modules/inputs/move-ish-input/module.json"
module_dsp="src/modules/inputs/move-ish-input/dsp/input_plugin.c"
build_file="scripts/build.sh"

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

require_file "$module_meta"
require_file "$module_dsp"

require_rg '"id"[[:space:]]*:[[:space:]]*"move-ish-input"' "$module_meta" "Move-ish metadata should use expected id"
require_rg '"name"[[:space:]]*:[[:space:]]*"Move-ish"' "$module_meta" "Move-ish should have a friendly name"
require_rg '"component_type"[[:space:]]*:[[:space:]]*"input_module"' "$module_meta" "Move-ish should be an input module"
require_rg '"led_mode"[[:space:]]*:[[:space:]]*"replace_pads"' "$module_meta" "Move-ish should default to pad LED replacement"
require_rg '"key"[[:space:]]*:[[:space:]]*"layout"' "$module_meta" "Move-ish should expose a layout parameter"
require_rg '"Chromatic"' "$module_meta" "Move-ish should expose native chromatic-style layout"
require_rg '"In-Key Octaves"' "$module_meta" "Move-ish should expose native in-key octave layout"
require_rg '"In-Key 4ths"' "$module_meta" "Move-ish should expose native in-key fourths layout"
require_rg '"key"[[:space:]]*:[[:space:]]*"offset"' "$module_meta" "Move-ish should expose pad-window offset"

require_rg "schwung_input_module_init_v1" "$module_dsp" "Move-ish should export input API init"
require_rg "LAYOUT_CHROMATIC" "$module_dsp" "Move-ish DSP should implement chromatic layout"
require_rg "LAYOUT_IN_KEY_OCTAVES" "$module_dsp" "Move-ish DSP should implement in-key octave layout"
require_rg "LAYOUT_IN_KEY_4THS" "$module_dsp" "Move-ish DSP should implement in-key fourths layout"
require_rg "chromatic_step = row \\* 5 \\+ col - 3 \\+ inst->offset" "$module_dsp" "chromatic layout should put root on bottom-row fourth pad with 3-note row overlap"
require_rg "scale_step = row \\* scale_len \\+ col \\+ inst->offset" "$module_dsp" "in-key octave layout should restart each row on the next octave"
require_rg "scale_step = row \\* 3 \\+ col \\+ inst->offset" "$module_dsp" "in-key fourths layout should offset rows by a diatonic fourth"
require_rg "strcmp\\(val, \"1\"\\)" "$module_dsp" "Move-ish should accept numeric enum index for In-Key Octaves"
require_rg "strcmp\\(val, \"2\"\\)" "$module_dsp" "Move-ish should accept numeric enum index for In-Key 4ths"
require_rg "note_from_scale_step" "$module_dsp" "Move-ish should map scale steps through current root and scale"
require_rg "held_note_matches" "$module_dsp" "Move-ish should light duplicate pads for held overlapping notes"
require_rg "ctx->root_note_class" "$module_dsp" "Move-ish should read root from input context"
require_rg "ctx->scale_name" "$module_dsp" "Move-ish should read scale from input context"
require_rg "g_host->get_track_color" "$module_dsp" "Move-ish should color roots/held notes with track color"
require_rg "COLOR_WHITE 120" "$module_dsp" "Move-ish should color in-key pads white"
require_rg "COLOR_OFF 0" "$module_dsp" "Move-ish should leave out-of-key chromatic pads off"
require_rg "request_led_redraw" "$module_dsp" "Move-ish should redraw LEDs on pad press/release"

require_rg "move-ish-input/dsp/input_plugin\\.c" "$build_file" "build should compile Move-ish DSP"
require_rg "build/modules/inputs/move-ish-input/dsp\\.so" "$build_file" "build should output Move-ish dsp.so"

echo "PASS: Stage 4 Move-ish input module hooks are present"
