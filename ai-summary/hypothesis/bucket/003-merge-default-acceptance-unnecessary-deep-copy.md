# H003: mergeCasesWithDefaultAcceptance deep-copies every entry unnecessarily

**Date**: 2025-07-21
**Subsystem**: bucket
**Severity**: Medium
**Impact**: all-level merge CPU, memory bandwidth — both serial (level 0) and background (levels 1-10)
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `mergeCasesWithDefaultAcceptance` retrieves an entry from the merge input
source, it should bind by `const&` to avoid copying. The entry is only passed
to `checkProtocolLegality` (reads type), `countOldEntryType` (reads type), and
`maybePut` (passes by `const&` to `putFunc`). None of these modify the entry,
so no copy is needed.

## Mechanism

In `BucketBase::mergeCasesWithDefaultAcceptance` (BucketBase.cpp:256-283),
two lines use `auto entry = ...` which deduces to a value type and deep-copies
the entry:

```cpp
// Line 259:
auto entry = inputSource.getOldEntry();   // deep copy!
// Line 275:
auto entry = inputSource.getNewEntry();   // deep copy!
```

Both `getOldEntry()` and `getNewEntry()` return `typename BucketT::EntryT const&`
(see BucketMergeAdapter.h:81-83, 87-89 for FileMergeInput; lines 152-161 for
MemoryMergeInput). The `auto` deduction strips the reference, creating a
full deep copy of each `BucketEntry`.

This function handles the **majority** of entries in any merge — those where
old and new have different keys (the "default acceptance" path). The
`mergeCasesWithEqualKeys` path, which handles the rarer same-key case,
correctly uses `BucketEntry const& oldEntry = inputSource.getOldEntry()`
(LiveBucket.cpp:261-262), avoiding the copy. This inconsistency suggests the
copy in `mergeCasesWithDefaultAcceptance` is unintentional.

For a BucketEntry containing a Soroban CONTRACT_DATA payload, the deep copy
includes allocating and copying nested `xdr::xvector<uint8_t>` buffers, which
can be hundreds of bytes to tens of KB per entry. For a level-10 merge
processing millions of entries, this is millions of unnecessary heap allocations
and potentially gigabytes of unnecessary memory copies.

## Trigger

Run any apply-load benchmark. Level 0 merges (every ledger, ~1000-10000 entries)
and background merges at deeper levels (periodically, potentially millions of
entries) all exercise this path. The impact scales linearly with total merge
entry count.

## Target Code

- `src/bucket/BucketBase.cpp:259` — `auto entry = inputSource.getOldEntry()` (copies instead of binding by reference)
- `src/bucket/BucketBase.cpp:275` — `auto entry = inputSource.getNewEntry()` (copies instead of binding by reference)
- `src/bucket/BucketBase.cpp:242-284` — full `mergeCasesWithDefaultAcceptance` function
- `src/bucket/LiveBucket.cpp:261-262` — `mergeCasesWithEqualKeys` correctly uses `const&` (consistent reference for comparison)
- `src/bucket/BucketMergeAdapter.h:81-89,152-161` — `getOldEntry()`/`getNewEntry()` return `const&`

## Evidence

1. `getOldEntry()` returns `const&` (BucketMergeAdapter.h:81-83,152-155), so
   `auto entry` deduces to `BucketEntry` (value) not `BucketEntry const&`.
2. The entry is used in read-only operations:
   - `checkProtocolLegality(entry, protocolVersion)` — reads `entry.type()`
   - `countOldEntryType(mc, entry)` — reads `entry.type()`
   - `maybePut(putFunc, entry, mc, ...)` — takes `BucketEntry const&`
3. `mergeCasesWithEqualKeys` (same file, same pattern) correctly avoids the
   copy using explicit `BucketEntry const&` type annotation (LiveBucket.cpp:261).
4. The `mergeCasesWithDefaultAcceptance` path handles every entry where the old
   and new keys differ. In a typical merge, this is 60-90% of all entries (the
   equal-key case only applies to entries modified between the two input bucket
   generations).
5. This affects ALL merge levels, including large background merges at deeper
   levels where millions of entries are processed. Background merges run on
   worker threads, so the wasted copies consume CPU that could be used by
   parallel transaction application.

## Anti-Evidence

1. The compiler _might_ optimize away the copy in some cases through return
   value optimization (RVO) or copy elision. However, the entry is bound by
   reference from the input source (the source continues to hold the original),
   so this is not a temporary — the compiler cannot legally elide this copy
   under C++17 rules. Only guaranteed elision applies to prvalues, not to
   lvalue-to-value copies.
2. For small classic entries (ACCOUNT ~200 bytes, TRUSTLINE ~300 bytes), the
   per-entry copy cost is relatively small. The impact is most significant for
   Soroban entries with large payloads.
3. If this has been present since the merge infrastructure was written, it
   hasn't caused observable issues in production — suggesting the absolute
   overhead may be modest relative to the I/O cost of file-based merges. But
   for the in-memory merge path (level 0) and for CPU-bound scenarios, the
   overhead is more significant.
