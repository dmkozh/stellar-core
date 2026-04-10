# H006: mergeInMemory double-checks protocol legality on every entry during output serialization

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: level-0 merge serial CPU on main thread
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `mergeInMemory` writes its merged entries to `BucketOutputIterator`, the
output path should not re-check protocol legality for entries that were already
validated during the merge pass. The merge itself already calls
`checkProtocolLegality` on every entry via `mergeCasesWithDefaultAcceptance`
(BucketBase.cpp:261,277), so the second check inside `BucketOutputIterator::put`
(BucketOutputIterator.cpp:84) is pure waste.

## Mechanism

`LiveBucket::mergeInMemory` (LiveBucket.cpp:549-613) performs two passes:

1. **Merge pass** (line 590): `mergeInternal` processes every entry from both
   input buckets. For each entry, `mergeCasesWithDefaultAcceptance` calls
   `LiveBucket::checkProtocolLegality(entry, protocolVersion)` at lines 261
   and 277 of BucketBase.cpp.

2. **Serialization pass** (lines 603-606): Iterates all `mergedEntries` and
   calls `out.put(e)` for each. Inside `put()` (BucketOutputIterator.cpp:84),
   `LiveBucket::checkProtocolLegality(e, mMeta.ledgerVersion)` is called again.

`checkProtocolLegality` does a protocol version comparison and a type check:
```cpp
if (protocolVersionIsBefore(protocolVersion, FIRST_PROTOCOL_...) &&
    (entry.type() == INITENTRY || entry.type() == METAENTRY))
```

On modern protocols this is always a quick false-return, but it still involves
a function call + two comparisons per entry. For a level-0 merge with thousands
of entries per ledger, this adds up to thousands of redundant checks on the
main-thread critical path.

Additionally, `put()` also performs the `BucketEntryIdCmp` comparison
(line 146-150) between consecutive entries to detect out-of-order writes.
Since `mergeInternal` already produces entries in sorted order, this ordering
check is also redundant for the merge output path. Together, these redundant
checks add ~3-5 function calls per entry in a path that should be a straight
serialize-and-hash.

## Trigger

Run any apply-load benchmark (sac, custom_token, soroswap). The cost scales
with the number of entries in level-0 per ledger. With ~3200 SAC transactions,
each producing ~6-10 entries, level-0 processes ~20K-30K entries per ledger.

## Target Code

- `src/bucket/LiveBucket.cpp:603-606` — serialization loop calling out.put(e)
- `src/bucket/BucketOutputIterator.cpp:78-165` — put() with redundant legality check and ordering check
- `src/bucket/BucketBase.cpp:256-283` — mergeCasesWithDefaultAcceptance already checks legality
- `src/bucket/LiveBucket.cpp:500-513` — checkProtocolLegality implementation

## Evidence

1. `mergeInternal` unconditionally calls `checkProtocolLegality` on every entry
   in `mergeCasesWithDefaultAcceptance` (BucketBase.cpp:261,277).
2. The merged entries are then written via `put()` which calls
   `checkProtocolLegality` again (BucketOutputIterator.cpp:84).
3. The ordering check `releaseAssert(!mCmp(e, *mBuf))` at line 146 is also
   redundant since mergeInternal guarantees sorted output.
4. The `mBuf` dedup logic (lines 142-165) maintains a one-entry buffer to
   deduplicate adjacent same-key entries. This is already handled by
   `mergeInternal` (which never emits duplicates), making the entire buffer
   mechanism unnecessary for merge output.

## Anti-Evidence

1. `BucketOutputIterator::put()` is also called from `fresh()` and file-based
   `merge()`, where the dedup/legality checks serve a purpose. A fix would
   need to either add a fast path for trusted merge output, or move the checks
   to be caller-responsibility.
2. The per-entry overhead of `checkProtocolLegality` on modern protocols is
   ~2-3 comparisons + function call overhead, totaling maybe 5-10ns per entry.
   With 20K entries, that's ~100-200μs — meaningful but not dominant against
   a 10-50ms close time.
3. The `mBuf` comparison involves `BucketEntryIdCmp` which does a multi-field
   comparison. This is more expensive per call (~20-50ns) and adds up to
   ~400μs-1ms for 20K entries, which is more significant.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated in fail/ or success/

### Trace Summary

Traced the full `LiveBucket::mergeInMemory` path (LiveBucket.cpp:549-613) confirming the two-pass pattern and redundant validation within `BucketOutputIterator::put()`. The merge pass (via `mergeInternal`) calls `checkProtocolLegality` on every entry at BucketBase.cpp:261,277. The serialization pass then calls `put()` which invokes `checkProtocolLegality` again at BucketOutputIterator.cpp:84, plus two `BucketEntryIdCmp` comparisons (lines 146,150) and a deep copy into `mBuf` (line 164) — all redundant for merge output since `mergeInternal` guarantees sorted, deduplicated, protocol-legal entries. The overhead is real but small relative to the dominant `writeOne` cost (XDR serialization + SHA256 hashing + buffered I/O).

### Code Paths Examined

- `src/bucket/LiveBucket.cpp:549-613` — `mergeInMemory`: confirmed two-pass pattern. Pass 1 (lines 584-591) populates `mergedEntries` via lambda. Pass 2 (lines 603-606) writes to BucketOutputIterator. Level 0 always merges with non-empty curr (shouldMergeWithEmptyCurr returns false for level 0 per BucketListBase.cpp:115,135), but after a snap, curr is an empty bucket.
- `src/bucket/BucketOutputIterator.cpp:76-165` — `put()`: Line 84 calls `checkProtocolLegality` (redundant). Lines 142-165 implement mBuf dedup: `releaseAssert(!mCmp(e, *mBuf))` (ordering check, redundant), `mCmp(*mBuf, e)` (always true for merge output since no duplicates), `*mBuf = e` (deep copy, redundant dedup buffer).
- `src/bucket/BucketBase.cpp:240-284` — `mergeCasesWithDefaultAcceptance`: Lines 261,277 call `checkProtocolLegality` on every entry during merge. This is the first check.
- `src/bucket/LiveBucket.cpp:500-513` — `checkProtocolLegality`: On modern protocols (≥11), the `protocolVersionIsBefore` check returns false immediately (~3-5ns with branch prediction).
- `src/util/XDRStream.h:483-515` — `writeOne()`: XDR serialization + SHA256 hash + buffered write. Cost per entry: ~400-1100ns depending on entry size. This dominates the per-entry cost in put().
- `src/bucket/BucketListBase.cpp:196-237` — `prepareFirstLevel`: Level 0 always uses in-memory merge path when curr has in-memory entries (line 215 guard). Creates fresh in-memory bucket (line 229-231), calls `mergeInMemory` (line 235).
- `src/bucket/BucketBase.cpp:392-399` — File-based `merge()`: writes directly from merge loop via putFunc calling out.put(), already the single-pass pattern.

### Findings

1. **Redundant checkProtocolLegality confirmed**: Called at BucketBase.cpp:261,277 during merge, then again at BucketOutputIterator.cpp:84 during output. On modern protocols, each call is ~3-5ns (fast-path branch). With ~30-48K entries per level-0 merge, this wastes ~90-240μs — negligible.

2. **Redundant mBuf dedup overhead is the main cost**: The two `BucketEntryIdCmp` comparisons (~40-60ns each) plus the `*mBuf = e` deep copy (~50-200ns for typical 100-500 byte entries) total ~130-460ns per entry. With 30-48K entries: ~3.9-22ms. However, against `writeOne` cost of ~400-1100ns per entry (12-53ms total for the write pass), this represents ~7-42% of the write pass time but only ~0.8-4.4% of total ledger close time (100-500ms for SAC at 3200 TX).

3. **Significant overlap with in-flight H001**: The reviewed hypothesis `001-single-pass-merge-serialize-in-mergeInMemory.md` addresses the same area by proposing to combine the two passes (putFunc does both emplace_back + out.put). If H001 is implemented, it eliminates the second traversal but NOT the per-put() validation overhead. H006's per-put() optimization would be incremental on top of H001.

4. **Fix approach — putUnchecked()**: Adding a `putUnchecked()` method to `BucketOutputIterator` that writes directly via `writeOne` without validation/dedup would save the full overhead. For `mergeInMemory` with `keepTombstoneEntries=true` (line 600), the tombstone elision check is always a no-op, so skipping it is safe. The sort-order assertion loss is acceptable given merge guarantees.

5. **Severity is Informational**: The realistic estimate of ~1-3% improvement on total close time is below the 5% threshold for Low severity. The mBuf copy cost is the dominant savings, but it competes with writeOne which does similar-sized data movement (XDR serialization traverses the same bytes).

### PoC Guidance

- **Target code**: `src/bucket/BucketOutputIterator.cpp` — add a `putUnchecked()` method; `src/bucket/LiveBucket.cpp:603-606` — call `out.putUnchecked(e)` instead of `out.put(e)` in the mergeInMemory serialization loop
- **Change description**: Add `putUnchecked()` that skips `checkProtocolLegality`, skips mBuf dedup, and writes directly via `mOut.writeOne(e, &mHasher, &mBytesPut); mObjectsPut++;`. Must still handle the mBuf finalization in `getBucket()` (set mBuf to nullptr or use a flag to indicate no pending buffer). Update mMergeCounters appropriately.
- **Correctness check**: Run `[bucket]` and `[bucketlist]` tag tests. Merge output must produce identical SHA256 hashes since the same entries are written in the same order. The putUnchecked path must NOT be used for `fresh()` or file-based `merge()` (only for the mergeInMemory serialization loop where entries are pre-validated).
- **Benchmark focus**: Run `sac` benchmark at TX=3200, T=1 with APPLY_LOAD_TIME_WRITES=true. Measure write-path time. Expect <3% improvement on total close time — likely within noise for end-to-end benchmarks. Best measured via micro-timing of the mergeInMemory function specifically.
