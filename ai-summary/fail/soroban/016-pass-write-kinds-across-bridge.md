# H002: Pass Write-Kind Metadata Across the Bridge to Skip Parallel Upsert Probes

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Low
**Impact**: CPU / redundant prestate lookups and deep copies during parallel writeback
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When Rust has already classified a storage diff as create, update, delete, or
restore relative to the invocation prestate, the C++ writeback path should use
that classification directly. It should not re-read the old entry from
`TxParallelApplyLedgerState` / `ThreadParallelApplyLedgerState` merely to decide
whether a write is a logical create or update.

## Mechanism

`get_ledger_changes` in the host bridge already knows whether an old entry
existed (`init_storage_snapshot.get(key)`), whether a new value exists, and
whether the key came from the restored set. But `extract_ledger_effects`
discards that metadata and returns only encoded new values plus synthetic TTL
entries. C++ therefore reconstructs create/update semantics by calling
`upsertLedgerEntry`, whose parallel path runs `TxParallelApplyLedgerState::upsertEntry`
and performs `getLiveEntryOpt(key)` solely to determine whether the entry
previously existed, copying old state from thread maps on every modified key.

## Trigger

Run batched SAC apply-load or any write-heavy `custom_token`/`soroswap`
scenario and sample `TxParallelApplyLedgerState::upsertEntry`,
`TxParallelApplyLedgerState::getLiveEntryOpt`, and
`ThreadParallelApplyLedgerState::getLiveEntryOpt` during
`InvokeHostFunctionOpFrame::recordStorageChanges`. If the hypothesis is
correct, a noticeable slice of post-host time will be spent probing old state
that Rust had already classified while building `LedgerEntryChange`.

## Target Code

- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs:get_ledger_changes:217-273` — already knows old-entry presence, new-entry presence, and restore status
- `src/rust/src/soroban_proto_any.rs:extract_ledger_effects:261-301` — throws away that classification and returns only encoded entries
- `src/transactions/InvokeHostFunctionOpFrame.cpp:recordStorageChanges:610-703` — rebuilds create/delete knowledge from bytes plus a RW-footprint scan
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::upsertEntry:907-950` — probes old state only to produce the "logical create" boolean
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::eraseEntryIfExists:953-975` — does the same for deletes

## Evidence

The host-side diff object already carries enough information to derive a compact
write kind. In the current C++ path, that information is thrown away, and the
parallel writeback layer pays an additional lookup/copy per modified key to
rediscover it. Batched SAC amplifies this because a single host invocation can
return O(100) balance writes plus TTL writes, causing the probe to repeat for
every modified entry in the batch.

## Anti-Evidence

The bridge change must carefully distinguish true creates from restores, because
restored keys intentionally reset "old size" accounting while not necessarily
representing the same TTL invariant as a brand-new key. Some writeback work
would remain even after removing the existence probe, so the win depends on the
probe being a meaningful fraction of `recordStorageChanges`.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (related to H014 index-write-effects-by-footprint but distinct mechanism)
**Failed At**: reviewer

### Trace Summary

Traced the full path from Rust `get_ledger_changes` (e2e_invoke.rs:175-275) through `extract_ledger_effects` (soroban_proto_any.rs:261-301), the CXX bridge, `recordStorageChanges` (InvokeHostFunctionOpFrame.cpp:610-703), and into `TxParallelApplyLedgerState::upsertEntry` (ParallelApplyUtils.cpp:907-950). Confirmed the inefficiency exists: `upsertEntry` calls `getLiveEntryOpt(key)` solely to return a bool indicating create-vs-update, and the scope adoption in that call chain performs a deep copy of the LedgerEntry that is immediately discarded after checking `.has_value()`. However, the actual cost is far smaller than the hypothesis implies.

### Code Paths Examined

- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs:get_ledger_changes:175-275` — Confirmed: knows old-entry presence via `init_storage_snapshot.get(key)` at line 217, and restored status via `restored_keys` at line 263. This info is embedded in `LedgerEntryChange` fields but only as `old_entry_size_bytes_for_rent` (zeroed for both creates AND restores at line 265).
- `src/rust/src/soroban_proto_any.rs:extract_ledger_effects:261-301` — Confirmed: discards all classification, returns flat `Vec<RustBuf>` of modified entries + synthesized TTL entries.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:recordStorageChanges:610-703` — Confirmed: rebuilds create/update via `upsertLedgerEntry(lk, le)` return value (line 655-658), builds `createdKeys` set, scans RW footprint for deletions (lines 689-702).
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::upsertEntry:907-950` — Confirmed: calls `getLiveEntryOpt(key)` at line 937-938, performs scope adoption deep copy, returns bool.
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::getLiveEntryOpt:886-904` — Checks `mTxEntryMap` (often has restore entries), falls through to `mThreadState.getLiveEntryOpt(key)`.
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::getLiveEntryOpt:700-735` — Checks `mThreadEntryMap` (pre-populated with footprint entries), usually hits there. Only deep-copies via `std::make_optional(*res)` if entry not in thread map.
- `src/ledger/LedgerEntryScope.cpp:scopeAdoptEntryOptFromImpl:444-457` — Confirmed: copies `entry.mEntry` (the `optional<LedgerEntry>`) to change scope tag, creating a temporary deep copy.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1001-1080` — Parallel apply helper pre-populates `mTxEntryMap` with restored entries via `upsertEntry` at lines 1064-1068, so those keys are found locally without thread-state lookup.

### Why It Failed

Three independent problems make this hypothesis not viable:

1. **The write-kind classification cannot be cleanly derived in the bridge layer.** `extract_ledger_effects` (bridge code, in-scope) receives `LedgerEntryChange` from `get_ledger_changes` (soroban-env-host, out-of-scope). The `LedgerEntryChange` struct has `old_entry_size_bytes_for_rent` which is zeroed for BOTH true creates AND restored entries (e2e_invoke.rs:265). To distinguish them, you need `restored_keys` which is only available inside `get_ledger_changes`. Adding a `was_created: bool` field to `LedgerEntryChange` requires modifying soroban-env-host internals, which is explicitly out of scope for this objective. TTL entries are also synthesized fresh in `extract_ledger_effects` (lines 280-296) and have no corresponding write-kind from the host.

2. **The actual cost is smaller than claimed.** For batched SAC (200 modified keys per tx): the `getLiveEntryOpt` path typically hits `mThreadEntryMap` (pre-populated hash map) on the first lookup, returning a reference without deep copy at the ThreadState level. The scope adoption copy (`scopeAdoptEntryOptFromImpl`) copies an `optional<LedgerEntry>`, but SAC balance entries are ~200-300 bytes and TTL entries ~40 bytes. Total copy cost: ~200 entries × ~200 bytes avg = ~40KB of copies at ~10 GB/s = ~4µs. Including hash map lookups (~200 × 2 × ~200ns = ~80µs) and hash set operations in `recordStorageChanges` (~100µs), total eliminable overhead is ~185µs per tx. Against ~5000µs host execution time, this is ~3.7% — well below the 5% threshold for Low severity.

3. **The `upsertEntry` return value serves a correctness invariant.** Lines 664-683 assert that every newly created Soroban entry has a matching TTL entry creation. While this could be replaced by bridge-provided metadata, removing the independent C++-side verification weakens defense-in-depth for a safety-critical system handling real monetary transactions.

### Lesson Learned

When a hypothesis proposes eliminating redundant classification work by passing metadata across the FFI bridge, verify (a) that the classification can actually be derived from the available `LedgerEntryChange` fields without modifying the host crate (out of scope), (b) that the restored-vs-created distinction is handled correctly, and (c) that the per-entry cost of the "wasted" work (a hash map lookup + small scope copy) is actually significant relative to the dominant costs (XDR deserialization, host execution). The scope adoption system adds a shallow copy, not a deep allocation-heavy clone, for entries already in the thread map.
