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
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::upsertEntry:907-950` — probes old state only to produce the “logical create” boolean
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
restored keys intentionally reset “old size” accounting while not necessarily
representing the same TTL invariant as a brand-new key. Some writeback work
would remain even after removing the existence probe, so the win depends on the
probe being a meaningful fraction of `recordStorageChanges`.
