#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q '#define MOD_MINIJV_INT_BLOCK_DIVIDER 4' "$file"; then
  echo "FAIL: missing Mini-JV modulation safety divider constant" >&2
  exit 1
fi
if ! rg -q 'static void chain_mod_apply_effective_value\(chain_instance_t \*inst, mod_target_state_t \*entry, int force_write\)' "$file"; then
  echo "FAIL: chain_mod_apply_effective_value force-write signature missing" >&2
  exit 1
fi
if ! rg -q 'strcmp\(inst->current_synth_module, "minijv"\) == 0' "$file"; then
  echo "FAIL: missing Mini-JV specific modulation throttle guard" >&2
  exit 1
fi
if ! rg -q 'if \(\(int\)entry->effective_value == \(int\)entry->last_applied_value\)' "$file"; then
  echo "FAIL: integer modulation dedupe guard missing" >&2
  exit 1
fi
if ! rg -q 'chain_mod_apply_effective_value\(inst, entry, 1\)' "$file"; then
  echo "FAIL: force-write restore path missing on modulation clear" >&2
  exit 1
fi

echo "PASS: chain modulation includes Mini-JV flood safeguards"
