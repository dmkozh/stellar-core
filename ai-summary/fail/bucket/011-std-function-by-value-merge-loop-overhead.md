# H011: std::function by-value in merge loop prevents inlining of putFunc

**Date**: 2025-07-21
**Subsystem**: bucket
**Severity**: Low
**Impact**: merge CPU overhead, both serial (level 0) and background (levels 1-10)
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

The merge hot loop in `mergeInternal` should invoke `putFunc` via direct call
dispatch, allowing the compiler to inline the function body (which is typically
a single `emplace_back` or `out.put()` call).

## Mechanism

`mergeCasesWithDefaultAcceptance` (BucketBase.cpp:245) and
`mergeCasesWithEqualKeys` (LiveBucket.cpp:194) accept `putFunc` as
`std::function<void(BucketEntry const&)>` by value. This means:

1. Each call to `mergeCasesWithDefaultAcceptance` from the `mergeInternal`
   loop (line 329) copies the `std::function` object (~32-64 bytes memcpy
   for SBO, once per entry).
2. The `putFunc(entry)` invocation goes through `std::function::operator()`
   which uses virtual dispatch (type-erased callable), preventing the
   compiler from inlining the actual function body.

For the in-memory merge (level 0), the lambda body is just
`mergedEntries.emplace_back(entry)` — a single vector push_back that the
compiler could trivially inline if it could see through the call.

## Trigger

Every merge at every level. The `mergeInternal` loop calls
`mergeCasesWithDefaultAcceptance` once per entry for the ~60-90% of entries
that have unequal keys.

## Target Code

- `src/bucket/BucketBase.cpp:245` — `std::function<void(...)> putFunc` by value in mergeCasesWithDefaultAcceptance
- `src/bucket/BucketBase.cpp:329-331` — mergeInternal passes putFunc to mergeCasesWithDefaultAcceptance
- `src/bucket/LiveBucket.cpp:194` — `std::function<void(...)> putFunc` by value in mergeCasesWithEqualKeys
- `src/bucket/LiveBucket.cpp:116-117` — maybePut also takes std::function by value

## Evidence

`mergeInternal` already templates `PutFuncT` (BucketBase.cpp:290:
`PutFuncT putFunc`), showing intent to support direct dispatch. But the
internal helper functions (`mergeCasesWithDefaultAcceptance`, `maybePut`)
immediately wrap it in `std::function`, negating the benefit.

## Anti-Evidence

The SBO in libstdc++/libc++ ensures the lambda (capturing 1 pointer, ~8 bytes)
fits in the inline buffer — no heap allocation per call. The indirect dispatch
overhead is ~2-5ns per call. For a level-0 merge with ~3000 entries, this is
~6-15μs total. Even for a multi-million-entry background merge, the indirect
call overhead is ~10ms against total merge times of seconds (dominated by I/O).
The `putFunc` body itself (disk write or vector emplace_back) is far more
expensive than the dispatch overhead.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-21
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The indirect dispatch overhead of `std::function` (~2-5ns per call) is
negligible relative to the work done per entry (XDR serialization, SHA256
hashing, disk write, or vector emplace_back). For level-0 merges (~3000
entries), total overhead is ~15μs. For deep-level merges (millions of entries),
the I/O dominates. Even a 10x reduction in dispatch overhead would produce
<0.01% improvement in ledger close time.

Templating the helper functions to avoid `std::function` would be the correct
fix but requires refactoring `mergeCasesWithDefaultAcceptance`,
`mergeCasesWithEqualKeys`, and `maybePut` as template functions (or inlining
them into `mergeInternal`), which adds template complexity for negligible
runtime benefit.

### Lesson Learned

In the merge path, the per-entry cost is dominated by XDR serialization, hashing,
and I/O — not by function dispatch. Optimizations should target the data
processing (copies, serialization passes) rather than the control flow overhead.
