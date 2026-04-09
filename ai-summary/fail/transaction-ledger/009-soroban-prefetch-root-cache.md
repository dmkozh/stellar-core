# H009: Remove Apply-Time Prefetch for Soroban Footprint Keys

**Date**: 2026-04-09
**Subsystem**: transaction-ledger
**Severity**: Low
**Impact**: Apply setup cache warming
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If apply-time prefetch were warming `LedgerTxnRoot` for Soroban footprint keys
that the parallel path never consults, removing that prefetch should reduce
serial setup work without changing parallel execution behavior.

## Mechanism

`prefetchTransactionData` runs before `applyTransactions`, so it initially
looked like a candidate for wasted cache warming: the parallel path mostly reads
from `InMemorySorobanState` and `ApplyLedgerStateSnapshot`, not from the root
entry cache. If Soroban footprint keys were being prefetched into the root
cache, that would be redundant work on the benchmark path.

## Trigger

Run any Soroban apply-load scenario and compare time in
`prefetchTransactionData` with the later parallel-apply lookup path.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2360-2376` — `prefetchTransactionData`
- `src/transactions/TransactionFrame.cpp:2008-2017` — `insertKeysForTxApply`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1322-1325` — Soroban invoke op prefetch hook
- `src/transactions/ExtendFootprintTTLOpFrame.cpp:366-369` — Soroban extend op prefetch hook
- `src/transactions/RestoreFootprintOpFrame.cpp:455-458` — Soroban restore op prefetch hook

## Evidence

- Prefetch runs once, serially, before any transaction application.
- The parallel apply path later resolves Soroban entries from thread-local state,
  `InMemorySorobanState`, or the apply snapshot rather than the root cache.

## Anti-Evidence

- `TransactionFrame::insertKeysForTxApply` only inserts op source accounts plus
  whatever the operation-specific hook adds.
- All three Soroban operation hooks (`InvokeHostFunction`, `ExtendFootprintTTL`,
  and `RestoreFootprint`) leave `insertLedgerKeysToPrefetch` empty, so Soroban
  footprint keys are never added to the prefetch set.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The suspected redundant work does not exist: apply-time prefetch is not warming
the root cache for Soroban footprint entries at all, because Soroban operations
do not contribute footprint keys to `insertKeysForTxApply`.

### Lesson Learned

Before blaming apply-time prefetch, verify the exact keys that
`insertKeysForTxApply` and `insertLedgerKeysToPrefetch` enqueue. In the Soroban
parallel path, the wasted setup work is elsewhere, not in root-cache prefetch of
footprint entries.
