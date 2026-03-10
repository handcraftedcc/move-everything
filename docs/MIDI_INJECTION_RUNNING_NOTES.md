# MIDI Injection Running Notes

Purpose: append-only notes for debugging `midi_to_move` injection stability in `src/move_anything_shim.c`.

## 2026-03-10

### Evidence observed
- Crash window showed mailbox occupancy spikes (`occ=3..5`, including slot 4) while injector activity continued.
- Crashes included assertion signal for event ordering and an additional assertion in `EventBuffer.hpp:233` (out-of-range event times).
- In external-injection mode, injector busy/interleave conditions were still causing queue drops.

### Change implemented
- External busy guard now blocks injection whenever the contiguous prefix is non-empty (`search_start > 0`), not only when 2+ slots are occupied.
- Busy return (`insert_rc == 2`) now defers queue processing to next cycle (keeps packet queued) instead of dequeueing/dropping.

### Tests
- Updated `tests/shadow/test_midi_to_move_injection_stability.sh` to assert:
  - guard condition is `search_start > 0`
  - busy path breaks/defer-retries
  - busy path does not advance `read_idx`
- Test result: PASS.

### Commit and deploy
- Commit: `835f51d` (`shim: defer midi injection when external mode sees busy prefix`)
- Branch pushed: `origin/MidiInTesting`
- Build/deploy artifact MD5: `5014374e4aaaa56066d3b2e5b6366dbd`
- Installed with: `./scripts/install.sh local --skip-confirmation --skip-modules`

## 2026-03-10 (internal mode parity)

### Evidence observed
- `midi_inject_test` was stable in external mode after busy-prefix defer changes.
- Crash could still occur in `source_mode=internal` (example repro: arp running while pads held).
- Root cause: strongest shim guard checks were keyed to external mode only.

### Change implemented
- Added queue mode flag: `SHADOW_MIDI_TO_MOVE_MODE_INTERNAL` in shared-memory header.
- `midi_inject_test` now publishes both external and internal mode flags based on current `source_mode`.
- Shim guard activation now uses a combined guard predicate (external OR internal mode enabled), so:
  - non-empty-prefix busy guard applies to internal mode
  - mailbox duplicate suppression applies to internal mode
  - internal-aftertouch suppression gate is enabled for guarded internal-forwarding windows

### Verification
- `tests/shadow/test_midi_to_move_injection_stability.sh`: PASS (updated to assert internal-flag plumbing + internal guard activation)
- Full build: PASS (`./scripts/build.sh`)

## 2026-03-10 (internal-mode dropout investigation)

### Evidence observed
- In `source_mode=internal`, users observed playback stopping after ~1-2s while arp/held notes were active.
- Shim diagnostics during repro showed healthy injector behavior (`inj` events present, `full=0`, `busy_drop=0`, no assertion spikes in sampled window).
- `midi_inject_test.log` showed repeated `drop reason=source src=2 mode=internal` (including note status), indicating source-filter mismatch upstream.

### Root cause
- Chain v1 path applies source gating via input mode before forwarding to synth.
- Chain v2 path (`v2_on_midi`) did not apply equivalent per-instance source gating before MIDI FX processing.
- Result: feedback/external-tagged events could still perturb arp/held-note state in internal-mode workflows.

### Change implemented
- Added per-instance source-gate helper in chain v2 path:
  - allow `MOVE_MIDI_SOURCE_HOST`
  - enforce `MIDI_INPUT_PADS` => internal only
  - enforce `MIDI_INPUT_EXTERNAL` => external only
- Applied gate in `v2_on_midi` before MIDI FX processing.

### Verification
- Added host regression test: `tests/host/test_chain_v2_midi_source_gate.sh` (PASS)
- Existing guard test: `tests/shadow/test_midi_to_move_injection_stability.sh` (PASS)

## Notes format for next entries
- `Date`
- `Evidence observed`
- `Hypothesis`
- `Change implemented`
- `Verification`
- `Open questions`

## 2026-03-10 (rollback of internal source-gate experiment)

### Evidence observed
- Internal mode regression reported after source-gating changes: `midi_inject_test` no longer forwarded expected internal MIDI stream while upstream arp still produced MIDI.
- External path remained stable, but internal forwarding behavior was worse than pre-change baseline.

### Change implemented
- Reverted two commits on `MidiInTesting`:
  - `d652cc1` (`chain: allow realtime clock through midi_inject_test gate`)
  - `96edaff` (`chain: prefilter midi_inject_test source before midi fx`)
- Revert commits:
  - `adef0c8` Revert "chain: allow realtime clock through midi_inject_test gate"
  - `e7be87c` Revert "chain: prefilter midi_inject_test source before midi fx"

### Verification
- Branch pushed with rollback state at `e7be87c`.
- Built rollback artifact successfully (`move-anything.tar.gz`, MD5 `6f8832ac48ae185607c767082f00e0a2`).
- Installed to device with:
  - `./scripts/install.sh local --skip-confirmation --skip-modules`

### Open questions
- Why held-note continuity in internal mode intermittently stops even when source gate regressions are removed.
- Whether intermittent stop aligns with clock/transport state transitions in arp path or with internal note lifecycle/state expiry.

## 2026-03-10 (internal-mode spotty output follow-up)

### Evidence observed
- User reported internal output still spotty/worse even after reverting source-gate experiment commits.
- `midi_inject_test.log` showed internal-mode forwarding bursts (`tx usb0`) followed by long quiet windows.
- Shim diagnostics during those windows showed repeated duplicate drops (`dup_drop=1`) with no busy/full pressure (`busy_drop=0`, `full=0`), pointing to duplicate-edge suppression rather than mailbox capacity.

### Hypothesis
- Internal arp/held-note streams can emit repeated note-ons for an already-held pitch before a matching note-off edge arrives.
- Global duplicate-edge suppression treats those repeated note-ons as duplicates and drops them, causing audible dropout in internal mode.

### Change implemented
- In `shadow_drain_midi_to_move_queue`, gated duplicate-edge suppression off for internal-only injection mode:
  - keep duplicate-edge behavior for external/both modes
  - bypass duplicate-edge drop when `internal_only_mode` is active

### Verification
- Added/updated regression assertion in:
  - `tests/shadow/test_midi_to_move_injection_stability.sh`
  - now requires duplicate-edge suppression to be gated for internal-only mode
- Test results:
  - `tests/shadow/test_midi_to_move_injection_stability.sh` PASS
  - `bash tests/host/test_chain_v2_midi_source_gate.sh` PASS

### Open questions
- Whether this resolves user-reported spotty internal continuity end-to-end on hardware.
- If any external-mode regressions appear, whether duplicate-edge policy needs a time-windowed filter instead of mode gating.

## 2026-03-10 (high-detail internal drop instrumentation)

### Evidence observed
- In a 150s clean capture after duplicate-edge gating, shim inject path remained healthy:
  - sustained injections, no `dup_drop`, no `busy_drop`, no `full`.
- Audible intermittency still reported while pads/arp were held.
- `midi_inject_test` showed intermittent `drop reason=source src=2 mode=internal` events (notably `0xA0`, and in prior windows also `0x90/0x80/0xF8`).

### Change implemented
- Added targeted verbose diagnostics in `midi_inject_test`:
  - In `source_mode=internal`, log every forwarded note edge (`0x90/0x80`) unsampled with source + note data.
  - In `source_mode=internal`, log unsampled source-mismatch drops for note-edge and realtime clock statuses (`0xF8`) including `d1/d2`.
- Kept sampled logging behavior for all other traffic to avoid excessive noise.

### Verification
- `tests/shadow/test_midi_to_move_injection_stability.sh` PASS
- `bash tests/host/test_chain_v2_midi_source_gate.sh` PASS

### Open questions
- Whether source-tag mismatches (`src=2` while mode is internal) align exactly with audible gaps in the new verbose note-edge stream.

## 2026-03-10 (chain v2 per-window MIDI path instrumentation)

### Evidence observed
- With shim and `midi_inject_test` verbose logs enabled, the injector path stayed healthy during intermittent audio dropouts:
  - sustained internal note-edge forwarding
  - no mailbox pressure (`busy_drop=0`, `full=0`)
  - no duplicate suppression in internal-only mode
- Intermittency persisted, implying drops are likely after ingress into chain v2 path (source gate / MIDI FX output / synth dispatch).

### Change implemented
- Added aggregated debug instrumentation in `src/modules/chain/dsp/chain_host.c` (`v2_on_midi`) guarded to `midi_inject_test` synth only:
  - 250ms window counters for input note edges/clock, source-gate blocks, MIDI-FX zero-output events, and synth-bound output message counts.
  - targeted `v2-midi blocked ...` logs for note-edge/clock packets rejected by source gating.
  - periodic `v2-midi ...` summary lines to correlate ingress vs egress volume through the chain path.

### Verification
- `tests/shadow/test_midi_to_move_injection_stability.sh` PASS
- `bash tests/host/test_chain_v2_midi_source_gate.sh` PASS

### Open questions
- During repro, do `v2-midi` window counters show input note-edge continuity while `out_note` falls to zero?
- If yes, is the loss concentrated in source-gate blocks (`blocked`) or MIDI FX zero-output (`fx_zero`) windows?
