# H008: Build DiskIndex metadata during bucket output instead of rescanning the finished file

**Date**: 2026-04-10
**Subsystem**: storage (bucket)
**Severity**: Medium
**Impact**: background merge CPU/I/O reduction on the apply path, strongest for T=8
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

For buckets large enough to use `DiskIndex`, index construction should reuse the
same ordered stream of entries and byte offsets that `BucketOutputIterator` is
already writing. The system should not close the file and then reopen it to
decode the entire bucket a second time just to rebuild range boundaries,
counters, filter inputs, and type ranges.

## Mechanism

`BucketOutputIterator::getBucket()` finishes writing the bucket, then calls
`createIndex(...)`. For large buckets, `LiveBucketIndex`/`HotArchiveBucketIndex`
construct `DiskIndex`, whose constructor reopens the just-written file,
iterates every entry with `XDRInputFileStream`, rebuilds counters and type
ranges, derives per-key filter hashes, and only then optionally persists the
index. This duplicates XDR decode work and adds a full read pass over merge
outputs that were already present in memory as ordered `BucketEntry` objects at
write time. In apply-load runs, those extra reads compete with SQLite writes,
bucket fsyncs, and other background merges on the same CPU and storage.

## Trigger

Run a long apply-load benchmark with writes enabled, especially T=8. Once
merge-output buckets exceed the default 20 MB disk-index cutoff, every large
level-1+ spill pays for bucket write + bucket reread + index-file write. The
effect compounds across repeated spills over 200 ledgers.

## Target Code

- `src/bucket/BucketOutputIterator.cpp:173-235` — closes the output file, then immediately calls `createIndex(...)`
- `src/bucket/DiskIndex.cpp:132-299` — reopens the bucket file and scans every entry to rebuild index metadata
- `src/main/Config.cpp:177-179` — defaults are `BUCKETLIST_DB_INDEX_CUTOFF = 20` MB and `BUCKETLIST_DB_PERSIST_INDEX = true`
- `docs/apply-load-benchmark-sac.cfg:13` — apply-load timings include writes

## Evidence

`BucketOutputIterator` already knows the exact write order and maintains
`mBytesPut`, so it has enough information to expose per-entry file offsets while
writing. The `DiskIndex` constructor's second pass is not discovering new
ordering information; it is reconstructing metadata that could be accumulated
online from the same `BucketEntry` stream. Eliminating the reread removes one
full XDR decode pass and one full read of every indexed merge output.

## Anti-Evidence

Large-bucket indexing happens on background merge threads, not always on the
foreground critical path, so the realized benchmark win depends on whether those
threads currently contend with close-time writes. Small buckets already use
`InMemoryIndex`, so the optimization only helps above the 20 MB cutoff.
Implementing an inline builder also requires threading offset/counter/filter
state through `BucketOutputIterator`, which is a nontrivial refactor.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full file-based merge path from `BucketBase::merge()` (BucketBase.cpp:340-427) through `BucketOutputIterator::getBucket()` (BucketOutputIterator.cpp:168-250) to `createIndex()` (BucketIndexUtils.cpp:30-51) to `DiskIndex` constructor (DiskIndex.cpp:132-300). Confirmed that for file-based merges (levels 1+), `getBucket()` is called with `inMemoryState=nullptr` (default), so there is no shortcut to `InMemoryIndex` construction from in-memory state. The code falls through to `createIndex()`, which reopens the just-written file and performs a full XDR decode pass to build the `DiskIndex`. The inefficiency is real, but its benchmark impact is limited by the fact that it runs on background merge threads and the file data is page-cache hot.

### Code Paths Examined

- `src/bucket/BucketBase.cpp:merge:340-427` — File-based merge calls `out.getBucket(bucketManager, &mk)` at line 426 with no `inMemoryState` argument (defaults to nullptr). This is the path for all level 1+ merges.
- `src/bucket/BucketOutputIterator.cpp:getBucket:168-250` — Lines 220-236: when `inMemoryState` is nullptr for LiveBucket, or always for HotArchiveBucket, falls through to `createIndex<BucketT>(bucketManager, mFilename, hash, mCtx, nullptr)` at line 233.
- `src/bucket/BucketIndexUtils.cpp:createIndex:30-51` — Constructs `LiveBucketIndex(bm, filename, hash, ctx, hasher)` or `HotArchiveBucketIndex` equivalent.
- `src/bucket/LiveBucketIndex.cpp:41-70` — Constructor checks `getPageSize()`. If bucket exceeds `BUCKETLIST_DB_INDEX_CUTOFF` (20MB), creates `DiskIndex<LiveBucket>` at line 67-68.
- `src/bucket/DiskIndex.cpp:132-300` — Constructor opens file with `XDRInputFileStream` (line 153-154), iterates ALL entries (line 169 `while (in && in.readOne(be, hasher))`), building: RangeIndex page boundaries (lines 233-244), BinaryFuseFilter key hashes (lines 228-231), AssetPoolIDMap (lines 193-221), BucketEntryCounters (line 246), type ranges (lines 188-191). Then optionally persists to disk via `saveToDisk()` (lines 296-299).
- `src/bucket/BucketOutputIterator.cpp:put:78-165` — During the write pass, `put()` already has access to each entry and tracks `mBytesPut` (line 153). All data needed for DiskIndex construction is available here.
- `src/bucket/InMemoryIndex.cpp:78-117` — The `InMemoryIndex(bm, inMemoryState, metadata)` constructor demonstrates the pattern: it builds index data from an in-memory vector without rescanning the file. This proves the pattern works.

### Findings

**The inefficiency is real and the proposed fix is architecturally sound.** The DiskIndex constructor performs a full second pass of XDR decode over the entire bucket file, rebuilding metadata that was available during the write. The `BucketOutputIterator::put()` method has access to each entry and the current file offset (`mBytesPut`), providing everything needed to accumulate DiskIndex metadata inline.

**Severity downgrade from Medium to Informational.** The hypothesis claims 10-20% benchmark improvement, but several factors limit the actual impact:

1. **Background threads, not critical path.** DiskIndex construction runs on background merge threads via `FutureBucket`. It only blocks the main thread when `BucketLevel::commit()` resolves a merge that hasn't completed yet. This happens infrequently (every 8 ledgers for level 1, every 32 for level 2, etc.).

2. **Page cache hot.** The bucket file was just written sequentially, so the OS page cache should contain the entire file. The rescan's I/O cost is essentially zero — the real cost is CPU time for XDR decode.

3. **CPU cost estimate.** For a 100MB bucket with ~100K entries at ~500ns XDR decode each: ~50ms per rescan. For a 50MB bucket: ~25ms. Over 200 ledgers with ~100 level-1 merges and ~25 level-2 merges, total rescan overhead is perhaps 2-5 seconds. Against a total benchmark time of 60-100 seconds, this is 2-8% — potentially Low severity, but the background-thread nature means not all of it translates to end-to-end improvement.

4. **Contention matters for T=8.** With 8 parallel application threads competing for CPU, the extra background CPU for DiskIndex rescans has more impact. This is where the optimization could shine, but quantifying contention effects requires benchmarking.

5. **The refactor is nontrivial but well-scoped.** `BucketOutputIterator` would need a new `DiskIndexBuilder` member that accumulates: page boundary entries, key hashes, AssetPoolID mappings, entry counters, and type ranges. At `getBucket()` time, the builder would construct the `BinaryFuseFilter` from accumulated hashes and assemble the `DiskIndex` without reopening the file.

### PoC Guidance

- **Target code**: `src/bucket/BucketOutputIterator.h` and `src/bucket/BucketOutputIterator.cpp` (add index metadata accumulation), `src/bucket/DiskIndex.h` and `src/bucket/DiskIndex.cpp` (add a constructor that accepts pre-built data), `src/bucket/LiveBucketIndex.cpp` and `src/bucket/HotArchiveBucketIndex.cpp` (add constructors accepting pre-built DiskIndex data)
- **Change description**: Add an optional `DiskIndexBuilder<BucketT>` to `BucketOutputIterator` that is activated when the output file is expected to exceed the DiskIndex cutoff. During `put()`, after `mOut.writeOne()`, accumulate: key into RangeIndex page map, key hash for BinaryFuseFilter, AssetPoolID if applicable, entry counter, type boundary. In `getBucket()`, if the builder is populated, construct DiskIndex from the accumulated data instead of calling `createIndex()`. Add a new `DiskIndex` constructor that accepts pre-built `DiskIndexData`.
- **Correctness check**: Run `./src/stellar-core test --ll fatal -r simple --abort --disable-dots "[bucket]"` — the bucket index tests verify index correctness. Also run `"[bucketindex]"` if that tag exists. The `DiskIndex::operator==` (BUILD_TESTS only) can verify that inline-built indexes match file-scan-built indexes.
- **Benchmark focus**: Run apply-load SAC 3200 T=8 benchmark. Add timing instrumentation around `createIndex()` calls in `getBucket()` to measure baseline rescan time. Expected improvement: 1-5% on T=8 scenarios due to reduced background CPU contention. The effect should be more visible on p99 ledger close times than medians.
