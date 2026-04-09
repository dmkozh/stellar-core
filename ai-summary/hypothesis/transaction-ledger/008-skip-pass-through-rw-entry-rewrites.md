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
