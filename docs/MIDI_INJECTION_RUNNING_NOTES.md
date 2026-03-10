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

## 2026-03-10 (internal drop deep-log correlation: superarp transport)

### Evidence observed
- Fresh repro capture after `d6c3a76` shows no injector pressure/failure signals:
  - `debug.log`: no assertions, no `v2-midi blocked`, no non-zero `busy_drop/full/dup_drop/mb_dup`.
  - shim diagnostics remain clean while intermittency is audible.
- `midi_inject_test.log` in internal mode still shows periodic long note-edge gaps (~1.5-1.9s), despite continuous bursts between gaps.
- New chain v2 counters in the same window show mostly non-note traffic (`last=src2 st=0xA0`), `in_clk=0`, and no source-gate blocks.
- Critical new signal from `/data/UserData/move-anything/superarp.log`:
  - repeated `MIDI Stop (internal)` events during active arp cycles
  - immediate all-notes-off flushes on each stop
  - frequent `MIDI Start (internal reset)` restarts afterwards
- Active slot state used in repro (`set_state/.../slot_2.json`) confirms:
  - synth `midi_inject_test`
  - MIDI FX `superarp` with `sync: "internal"`
  - chain input currently `"both"` (feedback traffic present in path)

### Interpretation
- The intermittent internal-mode drop is upstream of shim injection and likely tied to transport handling in `superarp` (or transport events reaching it), not mailbox occupancy/ordering.
- Repeated internal transport-stop handling matches the observed playback interruption pattern.

### Verification commands used
- Device logs copied locally:
  - `/tmp/midi_debug/debug.log`
  - `/tmp/midi_debug/midi_inject_test.log`
  - `/tmp/midi_debug/superarp.log`
- Key checks:
  - `grep -Eci 'ASSERT|assert|EventBuffer\\.hpp' /tmp/midi_debug/debug.log` => `0`
  - `grep -c 'v2-midi blocked' /tmp/midi_debug/debug.log` => `0`
  - `grep -Ec 'busy_drop=[1-9]|full=[1-9]|dup_drop=[1-9]|mb_dup=[1-9]' /tmp/midi_debug/debug.log` => `0`
  - `grep -c 'MIDI Stop (internal)' /tmp/midi_debug/superarp.log` => `105` (historical file; multiple recent occurrences observed near repro window)

### Open questions
- Which exact MIDI status/source combination is triggering `MIDI Stop (internal)` in the superarp path during this setup?
- Is this caused by internal transport forwarding, feedback path events when `input=both`, or superarp internal sync logic?

## 2026-03-10 (shim probe for external transport-stop packets)

### Evidence observed
- Superarp receives `MIDI Stop (internal)` events, but existing logs did not include packet cable/source at shim ingress.

### Change implemented
- Added temporary shim debug probe in `shadow_inprocess_process_midi`:
  - logs every incoming realtime `0xFC` packet with cable ID
  - distinguishes `cable=2` (`path=external-to-slots`) from non-external cables (`path=ignored-non-external`)

### Verification
- Pending hardware repro with updated build; expected debug lines:
  - `rt-stop observed status=0xFC cable=2 path=external-to-slots`
  - or non-external variants for cable 0/1.

### Open questions
- During the next dropout window, do `0xFC` packets appear on cable 2 right before superarp stop/reset events?

## 2026-03-10 (host probe for transport source attribution)

### Evidence observed
- Shim probe showed no `rt-stop observed ... cable=2` lines while `superarp.log` still reported `MIDI Stop (internal)`.
- This indicates stop events were not observed on shim external-cable forwarding path in the sampled run.

### Change implemented
- Added host-level transport probe in `src/move_anything.c` to log incoming MIDI transport statuses (`0xFA/0xFB/0xFC`) with cable and dispatch source:
  - `rt-transport midi_out ... cable=2 dispatch=external`
  - `rt-transport midi_out ... cable=0 dispatch=internal`
- Also logs `host_module_send_midi` transport injections (if any) for completeness.

### Verification
- Pending hardware repro with updated build to determine whether stop packets originate from:
  - internal MIDI_OUT stream (`cable=0`) vs
  - external feedback stream (`cable=2`) vs
  - explicit host_module_send_midi path.

### Open questions
- Does dropout-adjacent `0xFC` show up as `dispatch=internal` (likely XMOS/internal transport) or `dispatch=external` (feedback device path)?

## 2026-03-10 (host-transport repro: no Start/Stop observed, upstream cadence gaps)

### Evidence observed
- Fresh repro capture (after deploying host transport probe and resetting logs):
  - `/tmp/midi_debug/repro_20260310_113157/debug.log`
  - `/tmp/midi_debug/repro_20260310_113157/midi_inject_test.log`
- Transport probe results in `debug.log`:
  - `debug_rt_transport=0`
  - `debug_transport_status(FA/FB/FC)=0`
  - `debug_asserts=0`
- `midi_inject_test` internal-mode forwarding in the same repro window (`16:31`):
  - `tx note-edge src=0`: `317`
  - `tx note-edge src=2`: `0`
  - `drop reason=system`: `0`
  - `drop reason=queue-full`: `0`
  - `drop reason=source`: `1` (single `src=2 status=0xA0` mismatch while mode is internal)
- Internal forwarded note stream still shows periodic long cadence gaps despite no transport packets:
  - repeated ~`1.5s`-`1.9s` gaps between `tx note-edge src=0` events
  - examples: `16:31:18.630 gap_ms=1591`, `16:31:23.669 gap_ms=1649`, `16:31:59.221 gap_ms=1896`
- `superarp.log` was empty in this run (no correlated Stop lines available in that file).

### Interpretation
- In this repro, Start/Stop/Continue was not observed on either cable path (`internal` or `external`) and not via host injection path.
- Injection path remains healthy (no queue/system drops causing the pauses).
- Audible intermittency aligns with upstream internal note-generation cadence gaps before/at module ingress, not with shim mailbox pressure or transport-stop forwarding in this run.

### Open questions
- What upstream component/state causes the ~1.5s cadence gaps in internal-source note generation (e.g. MIDI FX settings/state, sync/rhythm config, or transport state machine not emitting FA/FB/FC)?

## 2026-03-10 (chain v2 gap detectors for internal-mode intermittent silence)

### Change implemented
- Added targeted debug-only gap detectors in `src/modules/chain/dsp/chain_host.c` (active only when synth is `midi_inject_test`):
  - `v2_on_midi` detector:
    - logs `v2-midi gap in-active-out-zero ...` when internal note-edge input continues but MIDI-FX output remains zero for `>800ms`.
    - logs `v2-midi gap recovered ...` when output resumes after a detected zero-output interval.
  - `v2_tick_midi_fx` detector:
    - logs `v2-midi gap tick-silent ...` when stream activity is recent but MIDI-FX tick emits no note edges for `>800ms`.
- Added per-instance debug state to track:
  - last internal note-edge input time
  - last note-edge output time
  - zero-output and tick-silent interval timing/counters

### Verification
- `tests/shadow/test_midi_to_move_injection_stability.sh` PASS
- `bash tests/host/test_chain_v2_midi_source_gate.sh` PASS

### Purpose
- Next repro should distinguish:
  - note input still arriving but MIDI-FX suppressing output, vs
  - MIDI-FX tick going silent despite recent stream activity.

## 2026-03-10 (prefx source guard for midi_inject_test internal mode)

### Motivation
- Logs showed chain patch input remained `midi_input=0` (`ANY`) while `midi_inject_test` was set to `source_mode=internal`.
- In that state, external-source traffic (`src=2`) could still reach MIDI FX (SuperArp) before being dropped later at synth forwarding.
- This allowed external feedback to perturb internal-mode MIDI FX timing/state.

### Change implemented
- Added a pre-MIDI-FX guard in `src/modules/chain/dsp/chain_host.c` (`v2_on_midi`):
  - if current synth is `midi_inject_test`
  - and synth `source_mode` resolves to `internal`
  - then drop `MOVE_MIDI_SOURCE_EXTERNAL` packets before MIDI FX processing.
- Added targeted debug line:
  - `v2-midi prefx block synth=... source_mode=internal src=2 status=...`

### Verification
- `tests/shadow/test_midi_to_move_injection_stability.sh` PASS
- `bash tests/host/test_chain_v2_midi_source_gate.sh` PASS

### Expected effect
- In `midi_inject_test` internal mode, SuperArp should no longer see external feedback packets.
- This should remove source-path contamination and make internal-mode behavior consistent with user intent.

## 2026-03-10 (transport-only prefx guard after internal-mode regression)

### Evidence observed
- Hardware repro with the broad pre-FX guard showed repeated:
  - `v2-midi prefx block synth=midi_inject_test source_mode=internal src=2 status=0xA0 ...`
- In the same window, internal-mode playback became starved/intermittent and `v2-midi gap tick-silent ...` continued.
- This confirmed the broad guard was over-filtering active note path traffic for this setup.

### Change implemented
- Narrowed the pre-FX guard in `src/modules/chain/dsp/chain_host.c`:
  - keep `midi_inject_test` + `source_mode=internal` condition
  - but only block external transport statuses `0xFA`/`0xFB`/`0xFC`
  - no longer block all external-source MIDI events.
- Added helper:
  - `is_external_transport_start_stop(const uint8_t *msg, int len)`
- Updated debug tag:
  - `v2-midi prefx block transport ...`

### Verification
- New host test added:
  - `tests/host/test_chain_midi_inject_prefx_guard.sh`
  - verifies guard is transport-only and checks `0xFA/0xFB/0xFC` handling
- Test results:
  - `bash tests/host/test_chain_midi_inject_prefx_guard.sh` PASS
  - `bash tests/host/test_chain_v2_midi_source_gate.sh` PASS
  - `tests/shadow/test_midi_to_move_injection_stability.sh` PASS
- Deployed to hardware (`./scripts/install.sh local`, modules/assets skipped).

### Current status after deploy
- Broad `prefx block ... src=2 status=0xA0` lines are gone.
- Internal-mode intermittency still reproduces (`v2-midi gap tick-silent ...`) and requires separate root-cause work in MIDI-FX/internal cadence path.

## 2026-03-10 (direct SuperArp instrumentation run)

### Change implemented
- Built a temporary instrumented `superarp` module from `move-anything-superarp` and deployed `dsp.so` to:
  - `/data/UserData/move-anything/modules/midi_fx/superarp/dsp.so`
- Instrumentation enabled SuperArp internal `dlog(...)` and added explicit tick-path no-output diagnostics:
  - `tick no-output sync=internal ...`
  - `tick no-output sync=clock ...`

### Repro evidence
- Capture window summary from `/data/UserData/move-anything/superarp.log`:
  - `MIDI Stop` count: `0`
  - `MIDI Start` count: `0`
  - `run_step emit` count: `153`
  - `tick no-output` count: `6305`
- Gap analysis on `run_step emit` sequence IDs:
  - `emit_count=153`
  - `max_seq_gap=52`
  - `large_gaps_ge_200=0`
- User noted one manual pad release during run; logs show corresponding state reset but no sustained scheduler stall.

### Interpretation
- In this capture, SuperArp itself did not stall and did not receive transport stop/start resets.
- `tick no-output` lines are expected between scheduled step boundaries; the observed cadence remained consistent.
- This points away from a SuperArp step scheduler freeze as the primary cause of intermittent playback in this run.

## 2026-03-10 (timestamp-correlated repro after SuperArp wall-clock log fix)

### Evidence observed
- Fresh repro capture (user held pads continuously; no deliberate burst input):
  - `/tmp/midi_debug/repro_20260310_141720/debug.log`
  - `/tmp/midi_debug/repro_20260310_141720/midi_inject_test.log`
  - `/tmp/midi_debug/repro_20260310_141720/superarp.log`
- No crash/transport/injector pressure signals in this run:
  - `debug_asserts=0`
  - `debug_rt_transport=0`
  - `debug_rt_stop_probe=0`
  - `debug_m2m_nonzero_drop=0` (`busy/full/dup/mb_dup` stayed zero)
  - `debug_v2_blocked=0`, `debug_v2_fx_zero_nonzero=0`
- `midi_inject_test` still showed recurring long note-edge gaps while in `source_mode=internal`:
  - examples: `19:16:44.857 gap_ms=1591`, `19:17:04.763 gap_ms=1536`, `19:17:20.640 gap_ms=1518`
- SuperArp log now includes wall-clock timestamps and shows matching long wall-time gaps between `run_step emit=1` entries, while sample-time remains one-step:
  - wall-time examples: `19:16:44.880 run_step_gap_ms=1614`, `19:17:20.640 run_step_gap_ms=2609`
  - same entries report `gap_samples=5504` (or occasional `5632`), i.e. normal per-step sample delta.
- Chain v2 gap detector also fired repeatedly:
  - `v2-midi gap tick-silent ... silent_ms~1000`
  - summary windows were dominated by `last=src2 st=0xA0 ...` traffic in this repro.

### Interpretation
- Intermittency is still upstream of shim injection stability guards.
- With `gap_samples` staying near one step while wall-clock gaps grow to ~1.5s+, the evidence indicates timing irregularity in when MIDI-FX tick work is being serviced in wall time (not a mailbox/order failure, and not transport-stop resets in this run).

### Change implemented
- Added a targeted chain debug probe in `src/modules/chain/dsp/chain_host.c` (`v2_tick_midi_fx`):
  - logs `v2-midi tick-gap ... dt_ms=... frames=...` when consecutive MIDI-FX tick calls are separated by `>=200ms` wall time (debug-only path for `midi_inject_test` instrumentation flow).

### Verification
- `tests/shadow/test_midi_to_move_injection_stability.sh` PASS
- `bash tests/host/test_chain_v2_midi_source_gate.sh` PASS

### Open questions
- Do the newly added `v2-midi tick-gap` events align exactly with the audible short-burst stall windows?
- If yes, what upstream scheduler/render path is delaying `v2_tick_midi_fx` servicing in this internal-mode setup?

## 2026-03-10 (root cause isolated: shim idle gate throttles MIDI-FX tick)

### Evidence observed
- Fresh repro with `v2-midi tick-gap` probe:
  - `/tmp/midi_debug/repro_20260310_142929/debug.log`
  - `/tmp/midi_debug/repro_20260310_142929/midi_inject_test.log`
  - `/tmp/midi_debug/repro_20260310_142929/superarp.log`
- Core counters in this run:
  - `debug_tick_gap=100`
  - `debug_tick_silent=16`
  - `debug_asserts=0`
  - `debug_m2m_nonzero_drop=0`
  - `debug_rt_transport=0`
  - `superarp_stop_internal=0`
- New probe output showed consistent render/tick gaps around half a second:
  - `v2-midi tick-gap ... dt_ms=496/497/499 frames=128`
  - distribution: `min=444ms`, `max=500ms`, `avg=494.7ms`
- In the same window:
  - `midi_inject_test` note-edge gaps remained ~`1.53s`-`1.68s`
  - `superarp` `run_step emit` wall-time gaps matched those pauses.
- Shim code review identified direct match:
  - per-slot idle gate in `shadow_inprocess_generate_audio` skips `render_block` while idle, probing only every 172 frames (`~0.5s`), which aligns with observed `tick-gap` cadence.

### Root cause
- For slots with MIDI FX (e.g. SuperArp), `tick()` is driven by chain `render_block`.
- The shim idle gate was using audio silence as a proxy for “safe to sleep” and skipping `render_block`, which starved MIDI-FX tick progression and produced intermittent burst/stall behavior.

### Change implemented
- In `src/move_anything_shim.c` idle-gate loop:
  - query `midi_fx_count` per active chain slot via `shadow_plugin_v2->get_param(..., "midi_fx_count", ...)`
  - if `midi_fx_count > 0`, force continuous render cadence for that slot:
    - bypass idle-skip branch
    - keep `shadow_slot_idle=0` / `shadow_slot_silence_frames=0`

### Verification
- `tests/shadow/test_midi_to_move_injection_stability.sh` PASS
- `bash tests/host/test_chain_v2_midi_source_gate.sh` PASS
- `bash tests/host/test_chain_midi_inject_prefx_guard.sh` PASS

### Open questions
- Hardware verify after this change:
  - do `v2-midi tick-gap` lines disappear (or drop sharply) in the same internal-mode held-pad repro?
  - do audible short-burst stalls stop with unchanged routing/settings?

## 2026-03-10 (idle cadence refinement: recent-activity window)

### Evidence observed
- Forcing continuous render whenever `midi_fx_count > 0` resolved SuperArp burst stalls, but it removed deep-idle render skipping for slots that were no longer receiving MIDI.
- Goal: preserve arp/tick continuity during active use while still allowing true-inactive slots to sleep.

### Change implemented
- Added per-instance MIDI-FX recent-activity tracking in chain v2 (`src/modules/chain/dsp/chain_host.c`):
  - new state: `midi_fx_last_activity_ms`
  - new window constant: `MIDI_FX_ACTIVE_WINDOW_MS` (`5000ms`)
  - activity is marked on incoming MIDI to chain (`v2_on_midi`) and when MIDI-FX tick emits output (`v2_tick_midi_fx`).
- Exposed new chain `get_param` key:
  - `midi_fx_active_recent` => `1` when activity was seen within the window, else `0`.
- Updated shim idle gate (`src/move_anything_shim.c`) to use `midi_fx_active_recent` instead of `midi_fx_count` for `force_continuous_render`.

### Verification
- Added host regression: `tests/host/test_chain_midi_fx_idle_activity_window.sh` (PASS).
- Existing regressions:
  - `tests/shadow/test_midi_to_move_injection_stability.sh` PASS
  - `bash tests/host/test_chain_v2_midi_source_gate.sh` PASS
  - `bash tests/host/test_chain_midi_inject_prefx_guard.sh` PASS

### Expected effect
- Active arp/internal injection sessions keep stable render/tick cadence.
- After inactivity, slots can return to idle skipping behavior instead of running continuously forever.
