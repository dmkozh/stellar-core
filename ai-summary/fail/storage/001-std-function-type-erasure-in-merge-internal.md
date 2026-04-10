# H002: std::function Type Erasure Overhead in mergeInternal for In-Memory Merge Path

**Date**: 2025-07-22
**Subsystem**: storage (bucket)
**Severity**: Informational
**Impact**: per-ledger CPU overhead from virtual dispatch in merge loop
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

The `putFunc` callback in `mergeInternal` should be inlined by the compiler
when the type is known at compile time. For the file-based merge path
(BucketBase.cpp:397), the compiler deduces a raw lambda type and can inline
the call. The in-memory merge path should behave identically.

## Mechanism

At BucketBase.cpp:429-434, the explicit template instantiation for the
in-memory merge path uses `std::function<void(BucketEntry const&)>` as the
`PutFuncT` type parameter:

```cpp
template void BucketBase<LiveBucket, LiveBucketIndex>::mergeInternal<
    MemoryMergeInput<LiveBucket>,
    std::function<void(BucketEntry const&)>>(...);
```

`std::function` introduces type erasure: each call goes through a virtual
dispatch (~5-15ns overhead) and prevents the compiler from inlining the
lambda body into the merge loop. The file-based merge at line 397 uses a
raw lambda (auto-deduced type), which the compiler can inline.

For ~10,000-16,000 entries per level-0 merge: 10,000 × 10ns = ~100-160μs
total overhead per ledger.

## Trigger

Every ledger close triggers `mergeInMemory` which instantiates `mergeInternal`
with the `std::function` specialization.

## Target Code

- `src/bucket/BucketBase.cpp:429-434` — explicit template instantiation with `std::function`
- `src/bucket/LiveBucket.cpp:585-588` — the `putFunc` lambda wrapped in `std::function`
- `src/bucket/BucketBase.cpp:289-425` — `mergeInternal` template that calls `putFunc`

## Evidence

1. The file-based merge at BucketBase.cpp:397 uses `auto putFunc = [&out](...) { out.put(e); }` — the compiler knows the exact type and can inline.
2. The in-memory merge at LiveBucket.cpp:585 wraps the lambda in `std::function<void(BucketEntry const&)>`, forcing type erasure.
3. `mergeInternal` is a hot loop calling `putFunc` for every output entry.

## Anti-Evidence

1. ~100-160μs per ledger against 100-500ms close time is <0.1% — well below the Informational threshold.
2. The `std::function` approach was likely chosen for simplicity and to avoid template bloat in the explicit instantiation.
3. Modern compilers may devirtualize `std::function` calls in some cases when the lambda is constructed in the same translation unit.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-22
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The total overhead is ~100-160μs per ledger (<0.1% of close time). Even if
completely eliminated, this is far below the 1% threshold for Informational
severity. The `std::function` type erasure is a micro-optimization that would
complicate the template instantiation machinery for negligible measurable
benefit.

### Lesson Learned

`std::function` overhead (~5-15ns per call) is only significant when called
millions of times per ledger. At ~10,000 calls per level-0 merge, the total
overhead is sub-millisecond and not worth optimizing in the apply path.
