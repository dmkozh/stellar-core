# H015: Cache Native Asset Contract ID in `TxEventManager::newFeeEvent`

**Date**: 2026-04-10
**Subsystem**: ledger, transactions
**Severity**: Informational
**Impact**: avoid per-fee-event native asset contract-ID hashing
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If transaction-level fee events are emitted in the apply-load benchmark,
`TxEventManager::newFeeEvent` should not recompute the native-asset contract ID
for every event. The native asset and network ID are constant for the lifetime
of the event manager, so repeated fee events should reuse a cached contract ID.

## Mechanism

`TxEventManager::newFeeEvent` calls `getAssetContractInfo(native, mNetworkID)`
for each event, which in turn computes `getAssetContractID(networkID, asset)`
via `xdrSha256` on a fresh `HashIDPreimage`. If fee events were enabled on the
benchmark path, caching the native SAC contract ID in `TxEventManager` would
remove a repeated hash-and-construct sequence from both the pre-fee and
post-refund event paths.

## Trigger

Run an apply-load benchmark with classic transaction events enabled and profile
`TxEventManager::newFeeEvent`.

## Target Code

- `src/transactions/EventManager.cpp:TxEventManager::newFeeEvent:603-625` — recomputes native asset contract info per fee event
- `src/transactions/TransactionUtils.cpp:getAssetContractInfo:2004-2018` — rebuilds balance/amount SCVals
- `src/transactions/TransactionUtils.cpp:getAssetContractID:2020-2029` — hashes a fresh `HashIDPreimage`
- `src/transactions/EventManager.cpp:classicEventsEnabled:12-20` — controls whether tx-level fee events are emitted at all
- `src/main/Config.cpp:338-339` — defaults `EMIT_CLASSIC_EVENTS` and `BACKFILL_STELLAR_ASSET_EVENTS` to false

## Evidence

- `newFeeEvent` computes native asset contract info on every call instead of
  storing it in the event manager.
- Both the pre-transaction fee event and the post-refund fee event route
  through this same helper.
- `getAssetContractID` constructs and hashes an XDR preimage on every call.

## Anti-Evidence

- `TxEventManager` is only enabled when `metaEnabled &&
  classicEventsEnabled(protocolVersion, config)` is true.
- `classicEventsEnabled` depends on `EMIT_CLASSIC_EVENTS` for current protocol
  versions, and `Config.cpp` initializes that flag to `false`.
- The apply-load benchmark configs disable metadata streaming and do not set
  `EMIT_CLASSIC_EVENTS`, so the benchmark keeps the default `false` and
  `newFeeEvent` returns immediately at `if (!mEnabled || amount == 0)`.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

This optimization is off the measured benchmark path. In the apply-load
configs, transaction-level classic events are not enabled, so
`TxEventManager::mEnabled` is false and `newFeeEvent` exits before any native
asset contract-ID hashing happens.

### Lesson Learned

For event-related apply-path optimizations, first confirm that the relevant
event manager is actually enabled by the benchmark configuration. BUILD_TESTS
forcing transaction meta on does not, by itself, imply that classic fee-event
construction is active.
