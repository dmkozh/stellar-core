# H006: Cache per-transaction host-function/auth/resource serialization

**Date**: 2026-04-10
**Subsystem**: transactions
**Severity**: Low
**Impact**: tx-local bridge marshaling
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If the same transaction had to serialize its host function, auth entries, Soroban resources, or source account multiple times on the measured apply path, a cached byte representation on `InvokeHostFunctionOpFrame` would be worth reusing. The apply path should only pay serialization when there is an actual second consumer of the same immutable payload.

## Mechanism

I considered caching `toCxxBuf(mInvokeHostFunction.hostFunction)`, `toCxxBuf(mResources)`, `toCxxBuf(mOpFrame.getSourceID())`, and auth-entry buffers on the operation frame. The expected reuse is not present: `doCheckValidForSoroban` does not serialize any of those objects, and `invokeHostFunction` is the only measured consumer, so the cache would mostly move one-off serialization work earlier without eliminating it.

## Trigger

Inspect the validation and apply flow for apply-load transactions, especially the invoke-host path used by SAC, custom-token, and Soroswap model txs.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::invokeHostFunction:526-553` — serializes host function, resources, source account, and auth entries once before the bridge call
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionOpFrame::doCheckValidForSoroban:1282-1310` — validation does not serialize those payloads
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSacPayments:2094-2148` — batch-transfer args vary by per-tx destination list
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateTokenTransfers:2323-2342` — transfer inputs vary by account pair
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSoroswapSwaps:3082-3206` — swap args/auth vary by account, pair, and direction

## Evidence

The only serialization of host-function inputs on the apply path happens inside `invokeHostFunction` immediately before the Rust bridge call. The benchmark generators also make these payloads transaction-specific: SAC batch transfers embed unique destination vectors, token transfers vary sender/receiver pairs, and Soroswap swaps vary the source account and path context.

## Anti-Evidence

The serialized blobs are immutable after tx construction, so a cache is technically correct. If a future path starts invoking the same host-function payload more than once per transaction, or if benchmark generators begin reusing identical auth/input blobs across many txs, the conclusion could change.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

There is no repeated consumer on the measured apply path: the payloads are serialized exactly once per transaction, and the benchmark transactions make those payloads mostly unique. A cache would add storage and invalidation complexity without removing a second serialization site.

### Lesson Learned

For bridge-marshaling ideas in apply-load, first prove that the same immutable bytes are consumed more than once. One-shot serialization inside a single invoke is a poor cache target unless many transactions can share the exact same blob.
