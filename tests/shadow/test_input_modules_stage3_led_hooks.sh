#!/usr/bin/env bash
set -euo pipefail

api_file="src/host/input_module_api_v1.h"
runtime_file="src/host/shadow_input_modules.c"
runtime_header="src/host/shadow_input_modules.h"
led_file="src/host/shadow_led_queue.c"
led_header="src/host/shadow_led_queue.h"
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

for f in "$api_file" "$runtime_file" "$runtime_header" "$led_file" "$led_header" "$chromatic_meta" "$chromatic_dsp"; do
  require_file "$f"
done

require_rg "set_pad_led" "$api_file" "input API should expose set_pad_led"
require_rg "get_pad_led" "$api_file" "input API should expose get_pad_led"
require_rg "get_track_color" "$api_file" "input API should expose get_track_color"
require_rg "get_root_note_class" "$api_file" "input API should expose root note class"
require_rg "get_scale_name" "$api_file" "input API should expose scale name"

require_rg "input_pad_owner_active" "$led_file" "LED queue should track input-module pad ownership"
require_rg "input_pad_snapshot_color" "$led_file" "LED queue should snapshot pad LEDs for input ownership"
require_rg "queue_input_pad_leds_restore" "$led_file" "LED queue should restore only input-owned pads"
require_rg "led_queue_set_input_pad_owner" "$led_file" "LED queue should expose scoped input pad owner control"
require_rg "led_queue_set_input_pad_led" "$led_file" "LED queue should expose module pad LED writes"
require_rg "led_queue_get_track_color" "$led_file" "LED queue should keep cached track color available for non-input callers"
require_rg "MOVE_PAD_NOTE_FIRST" "$led_file" "LED queue should use symbolic pad-note range"
require_rg "type == 0x90.*d1 >= MOVE_PAD_NOTE_FIRST.*d1 <= MOVE_PAD_NOTE_LAST" "$led_file" "LED queue should block native pad note LEDs only"
require_rg "queue_hw_leds_restore" "$led_file" "LED queue should continue reusing existing hardware restore flow"

require_rg "led_queue_set_input_pad_owner" "$runtime_file" "input runtime should toggle pad LED ownership"
require_rg "input_host_set_pad_led" "$runtime_file" "input runtime should wire set_pad_led"
require_rg "input_host_get_pad_led" "$runtime_file" "input runtime should wire get_pad_led"
require_rg "input_host_get_track_color" "$runtime_file" "input runtime should wire get_track_color"
require_rg "g_track_color\\[INPUT_TRACK_COUNT\\]" "$runtime_file" "input runtime should cache track colors from the set file"
require_rg "parse_track_colors_from_song" "$runtime_file" "input runtime should parse track colors from Song.abl"
require_rg "reset_track_colors" "$runtime_file" "input runtime should reset unknown track colors"
require_rg "return g_track_color\\[track_index\\]" "$runtime_file" "input runtime should return parsed set track colors"
require_rg "SONG_SET_POLL_USEC" "$runtime_file" "input runtime should poll Song.abl as the live key/scale source"
require_rg "input_song_file_poll" "$runtime_file" "input runtime should check Song.abl for live key/scale changes"
require_rg "input_queue_song_key_scale_update" "$runtime_file" "input runtime should queue Song.abl key/scale updates separately from Sentry"
require_rg "g_current_song_path" "$runtime_file" "input runtime should remember the active Song.abl path"
require_rg "g_song_mtime_sec" "$runtime_file" "input runtime should gate Song.abl parsing on file mtime"
require_rg "SONG_SET_HEADER_READ_LIMIT" "$runtime_file" "input runtime should cap live Song.abl reads to a small header"
require_rg "strstr\\(buf, \"\\\\\\\"tracks\\\\\\\"\"\\)" "$runtime_file" "input runtime should parse only top-level Song.abl data before tracks"
require_rg "input_update_led_ownership" "$runtime_file" "input runtime should centralize ownership transitions"
require_rg "read_module_led_mode" "$runtime_file" "input runtime should read module LED mode"
require_rg "replace_pads" "$runtime_file" "input runtime should support replace_pads mode"
require_rg "move_key_scale_state_t" "$runtime_file" "input runtime should maintain key/scale state"
require_rg "parse_key_scale_from_song" "$runtime_file" "input runtime should parse key/scale from the set"
require_rg "input_sentry_watcher_start" "$runtime_file" "input runtime should start the non-realtime key/scale watcher"
require_rg "shadow_input_update_key_scale_from_text" "$runtime_header" "input runtime should expose D-Bus text key/scale updates"
require_rg "input_key_scale_update_from_text" "src/host/shadow_dbus.h" "D-Bus host should accept key/scale text update callback"
require_rg "input_key_scale_update_from_text\\(text\\)" "src/host/shadow_dbus.c" "D-Bus text handler should feed input key/scale updates"
require_rg "\\.input_key_scale_update_from_text = shadow_input_update_key_scale_from_text" "src/schwung_shim.c" "shim should wire D-Bus text into input key/scale state"
require_rg "parse_screenreader_menu_item_text" "$runtime_file" "input runtime should parse native screen-reader menu item text"
require_rg "input_queue_screenreader_key_scale_update" "$runtime_file" "input runtime should queue screen-reader key/scale updates"
require_rg "SCREENREADER_DUPLICATE_USEC" "$runtime_file" "input runtime should dedupe duplicate screen-reader D-Bus messages"
require_rg "\\. Menu item\\. " "$runtime_file" "input runtime should recognize screen-reader menu-item suffixes"
require_rg "of 35" "$runtime_file" "input runtime should treat 35-item screen-reader menu as scale"
require_rg "of 12" "$runtime_file" "input runtime should treat 12-item screen-reader menu as root note"
require_rg "C♯/D♭" "$runtime_file" "input runtime should know native sharp/flat root text from screen reader"
require_rg "/data/UserData/Sentry" "$runtime_file" "input runtime should watch Move Sentry run directories"
require_rg "__sentry-breadcrumb1" "$runtime_file" "input runtime should read Sentry breadcrumb file 1"
require_rg "__sentry-breadcrumb2" "$runtime_file" "input runtime should read Sentry breadcrumb file 2"
require_rg "MAX_SENTRY_WATCH_FILES" "$runtime_file" "input runtime should track multiple Sentry breadcrumb files"
require_rg "input_sentry_poll_run_dir" "$runtime_file" "input runtime should poll every Sentry run directory"
require_rg "g_sentry_initial_scan_done" "$runtime_file" "input runtime should skip historical startup breadcrumbs but parse new run files"
if rg -q "g_sentry_files\\[2\\]" "$runtime_file"; then
  echo "FAIL: input runtime should not watch only one Sentry run directory" >&2
  exit 1
fi
require_rg "watch->mtime_nsec" "$runtime_file" "input runtime should detect rewritten fixed-size breadcrumb files"
require_rg "mtime_changed" "$runtime_file" "input runtime should parse breadcrumb changes even when file size does not grow"
require_rg "SENTRY_POLL_USEC 250000" "$runtime_file" "input runtime should use a slower Sentry-era watcher interval now that screen reader handles live key/scale"
if rg -q "input_sentry_poll\\(\\)" "$runtime_file"; then
  echo "FAIL: input runtime should not poll Sentry for live key/scale after screen-reader parsing" >&2
  exit 1
fi
require_rg "parse_sentry_key_scale_text" "$runtime_file" "input runtime may keep legacy Sentry key/scale parser for reference/fallback code"
require_rg "parse_sentry_embedded_set_to_messages" "$runtime_file" "input runtime should parse embedded binary Sentry message payloads"
require_rg "extract_sentry_timestamp_before" "$runtime_file" "input runtime should timestamp-gate reread Sentry payloads"
require_rg "if \\(!stamp \\|\\| !stamp\\[0\\]\\) return 0" "$runtime_file" "input runtime should reject embedded Sentry payloads without timestamps"
require_rg "c < 32 \\|\\| c >= 128" "$runtime_file" "input runtime should stop embedded Sentry values at binary separators"
require_rg "strncmp\\(start, \"♯\"" "$runtime_file" "input runtime should still allow unicode sharp root names"
require_rg "strncmp\\(start, \"♭\"" "$runtime_file" "input runtime should still allow unicode flat root names"
require_rg "g_last_sentry_scale_stamp" "$runtime_file" "input runtime should dedupe already-seen scale payloads"
require_rg "g_last_sentry_root_stamp" "$runtime_file" "input runtime should dedupe already-seen root payloads"
require_rg "best_scale_stamp" "$runtime_file" "input runtime should apply only the newest embedded scale payload per poll"
require_rg "best_root_stamp" "$runtime_file" "input runtime should apply only the newest embedded root payload per poll"
require_rg "input_pending_barrier" "$runtime_file" "input runtime should memory-order Sentry watcher to realtime handoff"
require_rg "Scale\\\\nset to " "$runtime_file" "input runtime should parse embedded Scale set-to payloads"
require_rg "Root note\\\\nset to " "$runtime_file" "input runtime should parse embedded Root note set-to payloads"
require_rg "Scale" "$runtime_file" "input runtime should parse native Scale user messages"
require_rg "Root note" "$runtime_file" "input runtime should parse native Root note user messages"
require_rg "set to " "$runtime_file" "input runtime should parse Sentry set-to message values"
require_rg "input_apply_pending_key_scale" "$runtime_file" "input runtime should apply Sentry updates from the realtime tick"
require_rg "notify_context_changed" "$runtime_file" "input runtime should trigger module redraw/context callbacks"
if rg -q "SENTRY_QUIET_USEC|input_stage_sentry_key_scale_update|input_flush_sentry_staged_updates" "$runtime_file"; then
  echo "FAIL: input runtime should not debounce key/scale updates after polling" >&2
  exit 1
fi
if rg -q "return led_queue_get_track_color" "$runtime_file"; then
  echo "FAIL: input runtime get_track_color should not depend on MIDI LED cache" >&2
  exit 1
fi

require_rg "shadow_input_tick_context" "$runtime_header" "input runtime should expose context tick"

require_rg '"led_mode"[[:space:]]*:[[:space:]]*"replace_pads"' "$chromatic_meta" "True Chromatic should default to pad LED replacement"
require_rg "draw_leds" "$chromatic_dsp" "True Chromatic should redraw its pad LEDs"
require_rg "g_host->set_pad_led" "$chromatic_dsp" "True Chromatic should use host set_pad_led"
require_rg "g_host->get_track_color" "$chromatic_dsp" "True Chromatic should use host track color"
require_rg "ctx->root_note_class" "$chromatic_dsp" "True Chromatic should color from host root note"
require_rg "ctx->scale_name" "$chromatic_dsp" "True Chromatic should color from host scale name"
require_rg "COLOR_WHITE 120" "$chromatic_dsp" "True Chromatic should use Move white for in-key notes"
require_rg "COLOR_OFF 0" "$chromatic_dsp" "True Chromatic should turn off off-key notes"
require_rg "c == '\\.'" "$chromatic_dsp" "True Chromatic should ignore punctuation in Move scale names like Whole-half Dim."
for scale in \
  "Harmonic Major" "Dorian #4" "Phrygian Dominant" "Lydian Augmented" \
  "Lydian Dominant" "Super Locrian" "8-Tone Spanish" "Bhairav" \
  "Kumoi" "Pelog Selisir" "Pelog Tembung" "Hungarian Minor" \
  "Messiaen 3" "Messiaen 4" "Messiaen 5" "Messiaen 6" "Messiaen 7"; do
  require_rg "$scale" "$chromatic_dsp" "True Chromatic should know captured Move scale: $scale"
done

echo "PASS: Stage 3 input pad LED ownership and True Chromatic coloring hooks are present"
