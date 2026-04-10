# H003: Shadow Vector Construction Waste in addBatchInternal Post-V12

**Date**: 2025-07-22
**Subsystem**: storage (bucket)
**Severity**: Informational
**Impact**: minor per-ledger allocation overhead
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When the protocol version is 12 or later, the `shadows` vector passed through
the level-spill cascade in `addBatchInternal` should be empty or not
constructed, since shadow-based merge elision was disabled in protocol 12
(FIRST_PROTOCOL_SHADOWS_REMOVED). The code should not build a vector of
bucket shared_ptrs that will never be used.

## Mechanism

In `BucketListBase::addBatchInternal` (BucketListBase.cpp:684-796), the code
constructs a `shadows` vector by iterating all 11 levels, pushing back both
`curr` and `snap` bucket shared_ptrs (22 `shared_ptr` copies). This vector is
then passed to each level's `prepare()` call. Inside `prepare()`, for protocol
≥12, `shouldMergeWithEmptyShadow()` returns true (BucketListBase.cpp:307-311),
so an empty shadow iterator vector is used instead and the `shadows` parameter
is ignored.

The waste: 22 `shared_ptr` copy operations (~2ns each × 22 = ~44ns) plus
vector allocation (~50-100ns) plus 11 `pop_back()` calls (~11ns).

Total: ~100-200ns per ledger.

## Trigger

Every ledger close calls `addBatchInternal` for the LiveBucketList.

## Target Code

- `src/bucket/BucketListBase.cpp:addBatchInternal:684-796` — shadow vector construction at lines ~745-770
- `src/bucket/BucketListBase.cpp:BucketLevel::prepare:290-330` — shadow parameter ignored for V12+

## Evidence

1. Protocol version is always ≥ 12 in modern networks (currently protocol 23).
2. The `shadows` vector is constructed unconditionally and then ignored by every `prepare()` call.
3. The `shouldMergeWithEmptyShadow()` function explicitly checks for V12+ and returns true.

## Anti-Evidence

1. The code is protocol-version-generic and must still work for old protocols in tests.
2. ~100-200ns per ledger is completely negligible — 6+ orders of magnitude below the ledger close time.
3. The shadow vector construction is outside the hot loop (once per ledger, not per entry).

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-22
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The total overhead is ~100-200ns per ledger. This is ~0.00002% of a 100ms
ledger close — many orders of magnitude below any measurable threshold.
The code correctly handles both pre-V12 and post-V12 protocols in a single
path, and the cost of maintaining this generality is essentially zero.

### Lesson Learned

Per-ledger O(1) operations with bounded small constants (e.g., 22 shared_ptr
copies across 11 BucketList levels) are never performance-relevant for the
apply path. Only per-entry or per-transaction operations at the ~10,000+
call scale warrant investigation.
