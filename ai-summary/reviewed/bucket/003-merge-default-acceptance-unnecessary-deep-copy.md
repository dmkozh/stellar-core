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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full merge path from `mergeInternal` (BucketBase.cpp:289-337) through `mergeCasesWithDefaultAcceptance` (BucketBase.cpp:242-284). Confirmed that `getOldEntry()`/`getNewEntry()` return `const&` in both `FileMergeInput` (BucketMergeAdapter.h:81-89, dereferencing `BucketInputIterator::mEntry`) and `MemoryMergeInput` (BucketMergeAdapter.h:152-161, indexing into a vector). Verified that `advanceOld()`/`advanceNew()` are called strictly after all uses of the entry, making a `const&` binding safe. Confirmed all downstream consumers (`checkProtocolLegality`, `countOldEntryType`/`countNewEntryType`, `maybePut`) accept `const&`.

### Code Paths Examined

- `src/bucket/BucketBase.cpp:242-284` — `mergeCasesWithDefaultAcceptance`: confirmed `auto entry = inputSource.getOldEntry()` at line 259 and `auto entry = inputSource.getNewEntry()` at line 275 create value copies due to `auto` stripping the reference
- `src/bucket/BucketMergeAdapter.h:81-89` — `FileMergeInput::getOldEntry()/getNewEntry()`: return `typename BucketT::EntryT const&` (reference to `BucketInputIterator::mEntry`)
- `src/bucket/BucketMergeAdapter.h:152-161` — `MemoryMergeInput::getOldEntry()/getNewEntry()`: return `typename BucketT::EntryT const&` (reference to vector element)
- `src/bucket/BucketInputIterator.h:29-31,50` — `mEntry` is the backing storage; `operator*` returns `const&` to it; `operator++` calls `loadEntry()` which overwrites `mEntry`
- `src/bucket/BucketInputIterator.cpp:155-165` — `operator++` calls `loadEntry()` which deserializes next entry into `mEntry`, invalidating prior references — but this only happens via `advanceOld()`/`advanceNew()` which are called after all uses of `entry`
- `src/bucket/LiveBucket.cpp:116-170` — `maybePut` takes `BucketEntry const& entry`, performs shadow checking (read-only on entry), then calls `putFunc(entry)`
- `src/bucket/HotArchiveBucket.cpp:80-85` — `maybePut` simply calls `putFunc(entry)`
- `src/bucket/LiveBucket.cpp:261-262` — `mergeCasesWithEqualKeys` correctly uses `BucketEntry const& oldEntry = ...`, confirming the inconsistency
- `src/bucket/LiveBucket.h:82,147,161-162` — `checkProtocolLegality`, `maybePut`, `countOldEntryType` all take `const&`

### Findings

The inefficiency is confirmed: `auto entry = inputSource.getOldEntry()` deep-copies every entry processed by the default acceptance path (60-90% of all merge entries). The fix — changing to `auto const& entry` — is trivially correct and safe:

1. **Lifetime safety**: `advanceOld()`/`advanceNew()` (which invalidate the underlying iterator reference) are called strictly after all uses of `entry` on both code paths (lines 264 and 280).
2. **No mutation**: All downstream consumers take `const&` — no function modifies the entry.
3. **Consistency**: `mergeCasesWithEqualKeys` already correctly uses `const&` for the same getOldEntry/getNewEntry calls.

**Severity downgrade from Medium to Informational**: While the inefficiency is real, the merge path is not a primary bottleneck in apply-load benchmarks. Level 0 merges are small (~3000 entries, synchronous on main thread) and already fast. Background merges (levels 1-10) run on worker threads and are dominated by XDR deserialization, SHA256 hashing, and I/O — the extra copy is a fraction of per-entry cost. The fix will reduce CPU and memory allocator pressure but is unlikely to produce a measurable improvement (>5%) on any benchmark scenario. The primary value is correctness/consistency with the rest of the merge code.

### PoC Guidance

- **Target code**: `src/bucket/BucketBase.cpp` lines 259 and 275
- **Change description**: Change `auto entry = inputSource.getOldEntry();` to `auto const& entry = inputSource.getOldEntry();` (line 259) and `auto entry = inputSource.getNewEntry();` to `auto const& entry = inputSource.getNewEntry();` (line 275). This eliminates one deep copy per entry for the 60-90% of entries that go through the default acceptance path.
- **Correctness check**: Run `[bucket]` tagged tests — specifically merge-related tests. The existing `BucketList` and merge tests cover this code path extensively. Also run `"BucketList.*"` and `"*merge*"` test patterns.
- **Benchmark focus**: Run apply-load benchmark (any scenario). Improvement will be subtle — look for reduced allocator pressure (fewer heap allocations) and modest reduction in merge CPU time. The improvement is most visible for merges involving large Soroban CONTRACT_DATA entries. Consider profiling with `perf` to measure allocation counts rather than wall-clock time.
