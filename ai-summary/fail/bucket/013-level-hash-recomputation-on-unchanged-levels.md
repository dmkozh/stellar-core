# H013: snapshotLedger recomputes SHA256 for unchanged bucket levels

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: per-ledger hash computation CPU in snapshotLedger
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When computing the bucket list hash in `snapshotLedger`, levels whose curr and
snap buckets have not changed since the last ledger should return their cached
hash without recomputing SHA256. Only levels that had a merge committed or a
snap rotation should need to update their hash.

## Mechanism

`BucketListBase::getHash()` calls `BucketLevel::getHash()` for each of the 11
levels. Each `BucketLevel::getHash()` computes
`SHA256::create() → add(mCurr->getHash()) → add(mSnap->getHash()) → finish()`.

This is a SHA256 computation over 64 bytes (two 32-byte hashes). While
`Bucket::getHash()` itself is cached (returns the stored hash), the per-level
SHA256 of (currHash || snapHash) is recomputed from scratch on every call to
`getHash()`.

On a typical ledger, only level 0 changes (and occasionally level 1 when level 0
spills). The other 9-10 levels produce the same hash as the previous ledger.

## Trigger

Run any apply-load scenario. `snapshotLedger` calls `getHash()` on every
ledger close.

## Target Code

- `src/bucket/BucketListBase.cpp:34-42` — `BucketLevel::getHash()` unconditionally
  computes SHA256 of (currHash || snapHash) for every level
- `src/bucket/BucketListBase.cpp:44-57` — `BucketListBase::getHash()` iterates
  all 11 levels calling `getHash()`
- `src/bucket/BucketManager.cpp:1106-1134` — `snapshotLedger` calls `getHash()`

## Evidence

SHA256-of-64-bytes costs approximately 300ns per invocation (one-block SHA256).
With 11 levels: 11 × 300ns = 3.3μs. The recomputation of 9-10 unchanged levels
wastes ~2.7-3.0μs per ledger. Additionally, each level call involves two
`shared_ptr<BucketT>::getHash()` method calls (fast, cached) and a SHA256
context creation/finalization.

## Anti-Evidence

At ~3.3μs per ledger close, this is ~0.03% of a 10ms close — deeply negligible.
Caching level hashes would require a dirty flag per level (set on commit or
snap), adding complexity for zero measurable benefit. The SHA256 library is
highly optimized and the one-block computation is effectively free.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The absolute cost of ~3.3μs for 11 SHA256 computations over 64 bytes each is
negligible by multiple orders of magnitude. Even doubling or tripling the
estimate to account for cache misses puts this well below 10μs — less than
0.1% of the cheapest ledger close. The optimization would save ~2.7μs (skipping
9-10 unchanged levels) at the cost of added complexity for dirty-flag tracking
per level.

### Lesson Learned

SHA256 of small fixed-size inputs (≤64 bytes) is effectively free (~300ns).
Hash recomputation over cached component hashes is never a meaningful target
unless the number of components is very large (thousands+). For the 11-level
BucketList, the total hash computation is below the noise floor of any
benchmark.
