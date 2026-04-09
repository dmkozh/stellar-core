# H008: Skip Decode-and-Upsert for Pass-Through RW Entries Returned by the Host

**Date**: 2026-04-09
**Subsystem**: transaction-ledger (transactions/InvokeHostFunctionOpFrame, transactions/ParallelApplyUtils, rust bridge)
**Severity**: Medium
**Impact**: Unnecessary mutation propagation after Soroban host return
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

RW entries that survive host execution without changing bytes should still count
for declared write resources, but they should not be decoded into `LedgerEntry`,
marked dirty, and propagated through the tx/thread/global maps as if they were
real ledger mutations.

## Mechanism

The Rust bridge documents that "ledger entries not returned have been deleted,"
so the returned `modified_ledger_entries` vector contains every surviving RW
entry, not just entries whose contents changed. C++ then decodes each returned
buffer in `recordStorageChanges`, validates it, and calls `upsertLedgerEntry`;
`TxParallelApplyLedgerState::upsertEntry` unconditionally inserts or updates the
tx-local map and bumps `lastModifiedLedgerSeq`. If many conservative RW
footprint entries are merely passed through unchanged, the apply path still pays
full decode, hash-set, dirty-map, merge, and commit costs for them.

## Trigger

Run `custom_token` and `soroswap` apply-load scenarios with `T=8`, and inspect
the ratio between host-returned RW entries and actually changed entries by
adding temporary counters around `recordStorageChanges`. The hypothesis is
strongest when contracts declare broader RW footprints than the subset they
mutate on a typical call.

## Target Code

- `src/rust/src/soroban_proto_any.rs:261-301` — `extract_ledger_effects` emits every surviving non-read-only entry, plus TTL updates
- `src/rust/src/soroban_proto_any.rs:304-309` — bridge contract states that omitted entries are interpreted as deletes
- `src/transactions/InvokeHostFunctionOpFrame.cpp:610-703` — `recordStorageChanges` decodes and upserts every returned entry
- `src/transactions/ParallelApplyUtils.cpp:907-950` — `TxParallelApplyLedgerState::upsertEntry` always creates a dirty tx-local entry and rewrites `lastModifiedLedgerSeq`

## Evidence

- The Rust-side output contract does not distinguish "unchanged but still
  present" from "changed"; both arrive as opaque entry bytes.
- C++ has no equality fast-path before `upsertLedgerEntry`, so identical output
  bytes still become logical writes.
- Any such pass-through entry also participates in later merge/commit work, not
  just initial decode.

## Anti-Evidence

- Some host functions may actually mutate nearly every RW entry, in which case
  there are few pass-through entries to skip.
- Resource accounting intentionally depends on RW footprints, so the fix must
  preserve fee / limit semantics even if it avoids logical rewrites.
- Implementing an efficient equality fast-path likely requires a bridge shape
  change (for example, returning RW indices or mutation flags), not just a local
  C++ tweak.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the complete data flow from the Soroban host's `get_ledger_changes`
(p26 `e2e_invoke.rs:183-292`) through the Rust bridge's `extract_ledger_effects`
(`soroban_proto_any.rs:261-301`) into C++ `recordStorageChanges`
(`InvokeHostFunctionOpFrame.cpp:610-703`) and `TxParallelApplyLedgerState::upsertEntry`
(`ParallelApplyUtils.cpp:906-951`). Confirmed the mechanism is correct: the host
sets `encoded_new_value = Some(entry_buf)` for ALL surviving RW entries regardless
of modification (e2e_invoke.rs:264-272), and C++ unconditionally decodes and
upserts each one. Then examined the RW footprints of all three benchmark workloads
to determine how many entries would actually be pass-through.

### Code Paths Examined

- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:183-292` — `get_ledger_changes` iterates all storage map entries; for `AccessType::ReadWrite`, if `entry_with_live_until_ledger` is `Some`, encodes the entry and sets `encoded_new_value = Some(entry_buf)` unconditionally
- `src/rust/src/soroban_proto_any.rs:261-301` — `extract_ledger_effects` pushes every non-read-only entry with `encoded_new_value` into `modified_entries`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:616-658` — `recordStorageChanges` loop: decodes XDR, validates, inserts into `createdAndModifiedKeys` set, calls `upsertLedgerEntry` for each entry
- `src/transactions/ParallelApplyUtils.cpp:906-951` — `upsertEntry` does `insert_or_assign` and sets `lastModifiedLedgerSeq` unconditionally
- `src/simulation/TxGenerator.cpp:738-812` — SAC individual transfer footprint: RW = {fromKey ACCOUNT, toKey CONTRACT_DATA}; both modified (XLM balance debit + balance entry create/credit)
- `src/simulation/TxGenerator.cpp:815-885` — custom_token transfer footprint: RW = {from balance CONTRACT_DATA, to balance CONTRACT_DATA}; both modified
- `src/simulation/TxGenerator.cpp:1490-1512` — SAC batch transfer footprint: RW = {source balance + N dest balances, all CONTRACT_DATA}; all modified
- `src/simulation/ApplyLoad.cpp:3134-3168` — soroswap swap footprint: RW = {2 TRUSTLINE, 2 SAC balance CONTRACT_DATA, 1 pair instance CONTRACT_DATA}; all 5 modified (user balances change, pair reserves update)
- `src/simulation/ApplyLoad.cpp:1132-1153` — SAC benchmark uses `makeNativeAsset()` (XLM), so ACCOUNT entry balance field is always modified by transfer

### Why It Failed

The mechanism described in the hypothesis is technically correct — the host does
return all surviving RW entries regardless of whether they were modified, and C++
does blindly decode and upsert each one. However, **in all three benchmark
workloads, every RW footprint entry is actually mutated by the host**, leaving
zero pass-through entries to skip:

1. **SAC individual transfer**: 2 RW entries (source ACCOUNT with XLM balance
   debited, destination CONTRACT_DATA balance credited/created) — both modified.

2. **SAC batch transfer**: N+1 RW entries (source CONTRACT_DATA balance + N
   destination CONTRACT_DATA balances) — all modified.

3. **custom_token transfer**: 2 RW entries (from CONTRACT_DATA balance, to
   CONTRACT_DATA balance) — both modified.

4. **soroswap swap**: 5 RW entries (2 user TRUSTLINEs with balances changed,
   2 pair SAC balance CONTRACT_DATA entries with balances changed, 1 pair instance
   CONTRACT_DATA with reserves updated) — all 5 modified.

Since there are zero pass-through entries in any benchmark scenario, the
optimization would produce exactly 0% improvement. Furthermore, implementing an
equality fast-path would add *overhead* (byte comparison on every entry) with no
entries to skip. The hypothesis's own anti-evidence ("some host functions may
actually mutate nearly every RW entry") is the reality for all benchmark
workloads.

### Lesson Learned

Before proposing a "skip unchanged entries" optimization, verify the actual
modification ratio in benchmark workloads. Soroban token transfer contracts
(SAC, custom_token) have tight RW footprints where every declared entry is
actually mutated. Soroswap has a larger footprint (5 RW entries) but still
modifies all of them during a swap. The optimization would only matter for
contracts that declare broad defensive RW footprints while only mutating a
small subset — a pattern not exercised by any current benchmark scenario.
