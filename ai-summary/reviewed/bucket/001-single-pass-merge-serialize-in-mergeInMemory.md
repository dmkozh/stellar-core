# H001: Single-pass merge and serialize in mergeInMemory eliminates redundant iteration and copy

**Date**: 2025-07-21
**Subsystem**: bucket
**Severity**: Low
**Impact**: ledger-close serial CPU, memory bandwidth
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When level-0 merge produces entries in `mergeInMemory`, the serialization to
disk and SHA256 hashing should happen in the same pass that builds
`mergedEntries`. There should be no second full iteration over the merged
entries, and no additional deep copy per entry into `BucketOutputIterator::mBuf`.

## Mechanism

`LiveBucket::mergeInMemory` performs two passes over the merged data:

1. **Pass 1** (lines 584-591): `mergeInternal` emits entries via `putFunc`, which
   copies each entry into `mergedEntries` via `emplace_back(entry)`.
2. **Pass 2** (lines 603-606): A `for` loop iterates all `mergedEntries`, calling
   `out.put(e)` for each one, which deep-copies into `BucketOutputIterator::mBuf`
   (line 164: `*mBuf = e`) before serializing to disk.

This creates two redundant costs:
- An entire extra O(N) traversal of the merged entries
- A deep copy of every BucketEntry into `mBuf` that exists only for the
  adjacent-dedup logic in `put()` — but `mergeInternal` already produces
  duplicate-free output, making the dedup buffer unnecessary

For the `sac` benchmark with ~3000+ state changes per ledger, this is ~3000
unnecessary deep copies of BucketEntry (which may contain Soroban CONTRACT_DATA
payloads of hundreds of bytes each) plus the overhead of the second traversal.

In contrast, the file-based `merge()` path (BucketBase.cpp:397-399) writes
directly from the merge loop: its `putFunc` calls `out.put(entry)` inline. The
in-memory path should achieve the same single-pass efficiency.

## Trigger

Run any apply-load benchmark (sac, custom_token, or soroswap) with
`APPLY_LOAD_TIME_WRITES=true`. Every ledger close invokes `mergeInMemory`
for level 0. The cost scales linearly with the number of state-changing entries
per ledger.

## Target Code

- `src/bucket/LiveBucket.cpp:584-612` — `mergeInMemory`: putFunc copies into mergedEntries (pass 1), then iterates mergedEntries to call out.put() (pass 2)
- `src/bucket/BucketOutputIterator.cpp:76-165` — `put()`: deep-copies entry into mBuf for dedup, writes previous mBuf entry via writeOne
- `src/bucket/BucketOutputIterator.cpp:167-250` — `getBucket()`: finalizes file, creates index, adopts bucket
- `src/util/XDRStream.h:482-515` — `writeOne()`: serializes entry to buffer, writes to file, optionally hashes

## Evidence

1. The second pass (lines 603-606) is a pure re-traversal that the file-based
   `merge()` avoids entirely by writing inline.
2. The `mBuf` dedup in `put()` is redundant for `mergeInternal` output, which
   guarantees no adjacent duplicates (the merge algorithm handles all key
   conflicts explicitly via `mergeCasesWithEqualKeys`).
3. The `mergedEntries` vector survives the output pass (it's moved into the
   bucket at line 612), so building it during the merge is unavoidable — but
   writing to the output stream can happen in the same pass.
4. A combined `putFunc` that both emplaces into `mergedEntries` AND writes to the
   XDR output stream (bypassing `mBuf`) would eliminate both the second iteration
   and the per-entry deep copy.

## Anti-Evidence

1. The `mBuf` in `BucketOutputIterator::put()` serves as an assertion that
   entries arrive in sorted order (line 146) and deduplicates adjacent same-key
   entries. Bypassing it for the in-memory path requires either a separate
   write method or confidence that `mergeInternal` never emits adjacent
   duplicates (which is guaranteed by design but not explicitly asserted in the
   output path today).
2. Level-0 buckets are small relative to deeper levels. In benchmarks with ~3000
   entries per ledger, the absolute savings is ~3000 deep copies × ~200-1000
   bytes each = ~0.6-3 MB of avoided memory bandwidth, plus iterator overhead.
   Against total ledger close times dominated by transaction execution, this is
   likely 1-3% of the close-path serial time.
3. This optimization only affects level 0 merges (every ledger). Background
   merges at deeper levels already use the efficient single-pass file-based
   path.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full `LiveBucket::mergeInMemory` path (LiveBucket.cpp:549-613) and confirmed the two-pass pattern: `mergeInternal` populates `mergedEntries` via `putFunc` (emplace_back copy), then a separate loop writes entries to `BucketOutputIterator::put()` (mBuf copy + serialization). Compared against the file-based `BucketBase::merge()` path (BucketBase.cpp:392-399) which constructs the output iterator first and writes directly from the merge loop in a single pass. The proposed fix of moving output iterator construction before `mergeInternal` and using a combined putFunc is correct and follows the exact pattern already used by file-based merges.

### Code Paths Examined

- `src/bucket/LiveBucket.cpp:549-613` — `mergeInMemory`: confirmed two-pass pattern. Pass 1 (lines 584-591) populates `mergedEntries` via `emplace_back`. Pass 2 (lines 603-606) iterates `mergedEntries` calling `out.put(e)`. Output iterator created at line 599-601 after merge completes. `mergedEntries` moved into bucket at line 610-612 (required for future in-memory re-merges).
- `src/bucket/BucketBase.cpp:392-399` — File-based `merge()`: output iterator constructed first (line 392), putFunc calls `out.put(entry)` directly (line 397-398), merge is single-pass. This is the pattern the in-memory path should follow.
- `src/bucket/BucketOutputIterator.cpp:28-74` — Constructor: opens temp file, writes METAENTRY. All constructor dependencies (tmpDir, meta, mc, ctx, doFsync) are available before mergeInternal in the in-memory path, so reordering is safe.
- `src/bucket/BucketOutputIterator.cpp:76-165` — `put()`: mBuf dedup and sort-order assertion. The `*mBuf = e` deep copy (line 164) occurs per entry. For mergeInternal output (guaranteed duplicate-free and sorted), the dedup logic does no real work but the copy and comparison still execute.
- `src/bucket/BucketBase.cpp:286-337` — `mergeInternal`: template taking `PutFuncT` by value. Calls `mergeCasesWithDefaultAcceptance` (which takes `std::function` by value — separate known overhead from H011). The putFunc receives entries by `const&`.

### Findings

1. **Two-pass pattern confirmed**: The in-memory merge does two full traversals where one suffices. The file-based merge already uses the single-pass pattern, proving it works.

2. **Copy count correction**: The hypothesis overstates the copy savings. In both the current two-pass and proposed single-pass approaches, each entry is copied exactly twice: once into `mergedEntries` (needed for in-memory bucket state) and once into `mBuf`/serialization. The savings come from eliminating the second traversal and improving cache locality, NOT from reducing copy count.

3. **Fix is straightforward and safe**: Move `LiveBucketOutputIterator` construction (currently lines 599-601) to before line 584. Change putFunc to both `emplace_back` into `mergedEntries` AND call `out.put(entry)`. Remove the separate loop (lines 603-606). All dependencies are satisfied: `meta`, `mc`, `ctx`, `doFsync` are all initialized before the merge.

4. **mBuf bypass is unnecessary and risky**: The simpler fix (just reordering, keeping `out.put()`) achieves the traversal elimination without losing the sort-order assertion. Bypassing mBuf would require a new write method and removes a useful correctness check.

5. **Impact is Informational, not Low**: The hypothesis estimates 1-3% of close-path serial time. With ~3000 entries at ~200-1000 bytes, the second traversal costs microseconds. The cache locality benefit is real but small. Against the severity scale requiring >5% for Low, this is Informational. It is unlikely to produce a measurable improvement in benchmarks.

### PoC Guidance

- **Target code**: `src/bucket/LiveBucket.cpp:mergeInMemory` (lines 584-612)
- **Change description**: Move the `LiveBucketOutputIterator out(...)` construction (lines 599-601) to before line 584. Change putFunc to:
  ```cpp
  auto putFunc = [&mergedEntries, &out](BucketEntry const& entry) {
      mergedEntries.emplace_back(entry);
      out.put(entry);
  };
  ```
  Remove the for-loop at lines 603-606. Leave the `getBucket()` call and `mergedEntries` move unchanged.
- **Correctness check**: Run `[bucket]` tag tests: `./src/stellar-core test --ll fatal -r simple --abort --disable-dots "[bucket]"`. Also run `[bucketlist]` tests. The merge output should be bit-identical (same SHA256 hash) since the same entries are written in the same order.
- **Benchmark focus**: Run `sac` benchmark at TX=3200, T=1. Measure write-path time (APPLY_LOAD_TIME_WRITES). Expect <1% improvement — likely within noise. The value is code cleanliness (consistency with file-based merge path) rather than measurable performance gain.
