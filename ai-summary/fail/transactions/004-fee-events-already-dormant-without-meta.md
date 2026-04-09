# H004: Fee event helpers are already dormant when transaction meta is off

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Low
**Impact**: tx-level fee event construction
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When transaction metadata output is disabled, the apply path should not spend
measurable time constructing fee `ContractEvent` payloads for Soroban
transactions. Calls to the fee-event helper should short-circuit before they
allocate topics, build asset-contract IDs, or append anything to per-tx event
buffers.

## Mechanism

I considered whether `applyParallelPhase` and post-tx refund processing still
paid for fee-event construction even with benchmark metadata disabled. The
actual behavior matches the expected behavior: `TxEventManager` disables itself
when `metaEnabled` is false, and `newFeeEvent` immediately returns before any
event-building work happens.

## Trigger

Run the apply-load benchmark with `METADATA_OUTPUT_STREAM = ""` in a normal
non-test build, then inspect the fee-event call sites in `applyParallelPhase`
and refund processing.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:LedgerManagerImpl::applyTransactions:2644-2653` — sets `enableTxMeta` from `ledgerCloseMeta != nullptr` (unless `BUILD_TESTS`)
- `src/ledger/LedgerManagerImpl.cpp:LedgerManagerImpl::applyParallelPhase:2747-2758` — still calls `newFeeEvent`, so viability depends on the callee's guard
- `src/transactions/EventManager.cpp:TxEventManager::TxEventManager/newFeeEvent:596-625` — `mEnabled` guard and immediate early return

## Evidence

`TxEventManager::TxEventManager` sets `mEnabled = metaEnabled &&
classicEventsEnabled(...)`, and `newFeeEvent` returns immediately when
`!mEnabled || amount == 0`. With metadata output disabled, the benchmarked
Soroban path reaches the call sites but does not construct `ContractEvent`
payloads.

## Anti-Evidence

`BUILD_TESTS` builds force `enableTxMeta = true`, so this path would become hot
again in test binaries even when the config disables metadata streaming.
However, that is not the stock benchmark configuration this objective targets.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The suspected overhead is already gated off by `TxEventManager` itself: the
benchmark reaches `newFeeEvent`, but the helper returns before doing any
meaningful work when tx meta is disabled.

### Lesson Learned

For metadata-related apply-load hypotheses, check the event-manager enable flag
before assuming that a visible call site performs real work. Several event
helpers remain in the hot path syntactically while becoming true no-ops at
runtime.
