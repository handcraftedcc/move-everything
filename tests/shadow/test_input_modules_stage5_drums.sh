#!/usr/bin/env bash
set -euo pipefail

api_file="src/host/input_module_api_v1.h"
runtime_file="src/host/shadow_input_modules.c"
runtime_header="src/host/shadow_input_modules.h"
shim_file="src/schwung_shim.c"
module_meta="src/modules/inputs/drums-input/module.json"
module_dsp="src/modules/inputs/drums-input/dsp/input_plugin.c"
build_file="scripts/build.sh"

require_file() {
    local path="$1"
    if [[ ! -f "$path" ]]; then
        echo "FAIL: missing $path" >&2
        exit 1
    fi
}

require_rg() {
    local pattern="$1"
    local path="$2"
    local message="$3"
    if ! rg -q "$pattern" "$path"; then
        echo "FAIL: $message" >&2
        echo "  missing pattern: $pattern" >&2
        echo "  in: $path" >&2
        exit 1
    fi
}

require_file "$module_meta"
require_file "$module_dsp"

require_rg "schedule_midi" "$api_file" "input API should expose scheduled MIDI for timed input modules"
require_rg "cancel_scheduled_midi" "$api_file" "input API should let held ratchets cancel future scheduled hits"
require_rg "on_tick" "$api_file" "input API should expose a realtime tick callback for held gestures"
require_rg "input_schedule_midi" "$runtime_file" "input runtime should implement scheduled MIDI callback"
require_rg "input_cancel_scheduled_midi" "$runtime_file" "input runtime should implement scheduled MIDI cancellation"
require_rg "input_drain_scheduled" "$runtime_file" "input runtime should drain scheduled MIDI from the input tick path"
require_rg "INPUT_SCHEDULED_EVENTS" "$runtime_file" "input runtime should bound scheduled MIDI queue size"
require_rg "get_transport_playing" "$runtime_header" "input host should expose transport playing"
require_rg "sampler_transport_playing" "$shim_file" "shim should feed real transport state to input modules"

require_rg '"id"[[:space:]]*:[[:space:]]*"drums-input"' "$module_meta" "Drums metadata should use expected id"
require_rg '"name"[[:space:]]*:[[:space:]]*"Drums"' "$module_meta" "Drums should have a friendly name"
require_rg '"component_type"[[:space:]]*:[[:space:]]*"input_module"' "$module_meta" "Drums should be an input module"
require_rg '"led_mode"[[:space:]]*:[[:space:]]*"replace_pads"' "$module_meta" "Drums should default to pad LED replacement"
require_rg '"key"[[:space:]]*:[[:space:]]*"mode"' "$module_meta" "Drums should expose a mode parameter"
require_rg '"32 Drums"' "$module_meta" "Drums should expose 32-pad mode"
require_rg '"16 Velocities"' "$module_meta" "Drums should expose velocity mode"
require_rg '"16 Ratchets"' "$module_meta" "Drums should expose ratchet mode"
require_rg '"key"[[:space:]]*:[[:space:]]*"base_note"' "$module_meta" "Drums should expose base note"

require_rg "schwung_input_module_init_v1" "$module_dsp" "Drums should export input API init"
require_rg "MODE_32_DRUMS" "$module_dsp" "Drums DSP should implement 32-pad mode"
require_rg "MODE_16_VELOCITIES" "$module_dsp" "Drums DSP should implement velocity mode"
require_rg "MODE_16_RATCHETS" "$module_dsp" "Drums DSP should implement ratchet mode"
require_rg "left16_note" "$module_dsp" "Drums should map the left 4x4 pads as drum notes"
require_rg "velocity_for_pad" "$module_dsp" "Drums should map right-side pads to velocity levels"
require_rg "ratchet_rates" "$module_dsp" "Drums should define ratchet rate columns"
require_rg "ratchet_patterns" "$module_dsp" "Drums should define ratchet pattern rows"
require_rg "\\{1, 1, 1, 1, 0, 0\\}" "$module_dsp" "Ratchet row 1 should be XXXX"
require_rg "\\{1, 1, 1, 0, 0, 0\\}" "$module_dsp" "Ratchet row 2 should be XXXO"
require_rg "\\{1, 1, 0, 0, 0, 0\\}" "$module_dsp" "Ratchet row 3 should be XXOO"
require_rg "\\{1, 1, 1, 1, 0, 0\\}" "$module_dsp" "Ratchet row 4 should be XXXXOO"
require_rg "g_host->schedule_midi" "$module_dsp" "Ratchets should use scheduled MIDI rather than a worker thread"
require_rg "ctx->bpm" "$module_dsp" "Ratchets should derive timing from input context BPM"
require_rg "ctx->playing" "$module_dsp" "Ratchets should have access to transport playing state"
require_rg "on_tick" "$module_dsp" "Ratchets should continue scheduling while held"
require_rg "ratchet_active" "$module_dsp" "Drums should track held ratchet pads"
require_rg "cancel_scheduled_midi" "$module_dsp" "Ratchet release should cancel future scheduled hits"
require_rg "int velocity = inst->fixed_velocity \\? 100 : in->data2" "$module_dsp" "Ratchets should use pad press velocity, not the velocity grid"
require_rg "type == 0xA0" "$module_dsp" "Ratchets should react to pad afterpressure packets"
require_rg "inst->ratchet_velocity = clamp_int\\(in->data2, 1, 127\\)" "$module_dsp" "Ratchets should modulate scheduled velocity from afterpressure"
require_rg "ratchet_pattern_lengths" "$module_dsp" "Ratchets should support non-4-step patterns like XXXXOO"

require_rg "request_led_redraw" "$runtime_file" "input runtime should honor module-requested LED redraws"
require_rg "led_queue_set_input_pad_owner\\(0\\)" "$runtime_file" "LED mode changes should restore native pad LEDs when replacement is disabled"
require_rg "notify_context_changed\\(track\\)" "$runtime_file" "LED mode changes should force modules to redraw replacement pad LEDs"
require_rg "normalizeInputParamValue" "src/shadow/shadow_ui_input_modules.mjs" "input UI should normalize enum labels to stored values"
require_rg "input_module:param:\" \\+ key" "src/shadow/shadow_ui_input_modules.mjs" "input UI should send LED mode param changes to the shim"

require_rg "drums-input/dsp/input_plugin\\.c" "$build_file" "build should compile Drums DSP"
require_rg "build/modules/inputs/drums-input/dsp\\.so" "$build_file" "build should output Drums dsp.so"

echo "PASS: Stage 5 Drums input module and scheduling hooks are present"
