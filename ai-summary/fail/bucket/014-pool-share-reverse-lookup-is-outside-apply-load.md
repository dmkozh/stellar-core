# H014: Pool-share trustline reverse lookup is outside the apply-load benchmark

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: out-of-scope bucket query path
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Optimization work for this objective should target code that materially affects
the `sac`, `custom_token`, and `soroswap` apply-load scenarios. A bucket query
that is only used by classic pool-share revocation helpers should be excluded,
even if its local implementation looks expensive.

## Mechanism

`SearchableLiveBucketListSnapshot::loadPoolShareTrustLinesByAccountAndAsset()`
does two BucketList passes and builds a potentially large set of trustline keys,
which initially made it look like a promising bucket optimization. But the call
chain is rooted in `prefetchPoolShareTrustLinesByAccountAndGetKeys()` inside the
classic revoke / sponsorship cleanup utilities, not in the apply-load benchmark
workloads named in this objective.

## Trigger

Invoke the classic pool-share revocation path that removes offers and pool-share
trustlines for an issuer-backed asset. This is not the hot path exercised by the
apply-load benchmark matrix.

## Target Code

- `src/bucket/BucketListSnapshot.cpp:462-499` — reverse lookup builds `trustlinesToLoad` via `getPoolIDsByAsset(...)` and then bulk-loads those keys
- `src/bucket/LiveBucketIndex.cpp:260-282` — every bucket contributes `PoolID` candidates for the asset
- `src/transactions/TransactionUtils.cpp:1475-1518` — helper prefetches extra pool-share-related state
- `src/transactions/TransactionUtils.cpp:1550-1583` — only call path found for `loadPoolShareTrustLinesByAccountAndAsset(...)`

## Evidence

The bucket-side implementation really does perform two whole-bucket passes: one
to collect candidate pool IDs and another to load the derived trustline keys.
But the only production call path found is the pool-share revocation helper in
`TransactionUtils.cpp`, which sorts and returns those keys for classic cleanup
flows rather than for the benchmarked Soroban apply-load scenarios.

## Anti-Evidence

If a future benchmark adds issuer revocation or pool-share sponsorship cleanup,
this path could become relevant. Right now, under the stated objective, it is a
distraction from hotter apply-path work.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The code path is not part of the apply-load benchmark target. Improving it would
optimize a niche classic pool-share helper rather than the ledger-apply paths
measured by `sac`, `custom_token`, and `soroswap`.

### Lesson Learned

Bucket query helpers can look expensive in isolation, but they still need a
confirmed edge into the benchmarked workload. For this objective, verify an
`ApplyLoad` or pre-apply call path before spending hypothesis budget on
specialized pool-share helpers.
