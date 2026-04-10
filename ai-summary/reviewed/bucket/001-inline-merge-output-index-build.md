# H001: Build merge-output indexes inline instead of rescanning finished bucket files

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Medium
**Impact**: background merge CPU/I/O and multi-threaded apply tail latency
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once a merge or fresh-bucket path has already streamed every output entry through
`BucketOutputIterator`, index construction should reuse that same pass. The code
should not close the just-written bucket file and immediately reopen it to
re-parse every XDR record solely to rebuild range boundaries, bloom-filter
inputs, counters, and pool-ID metadata that were already available while the
entries were being emitted.

## Mechanism

`BucketOutputIterator::getBucket` closes the output file and, unless it can
reuse an existing index or a caller-supplied in-memory state, calls
`createIndex()`. That path constructs `LiveBucketIndex`/`HotArchiveBucketIndex`,
which reopen the finished bucket file and iterate through it again
entry-by-entry; large live buckets then do an additional persisted-index write.
For apply-load this means spill-driven merge outputs pay one full write/XDR pass
to create the bucket and then a second full read/XDR pass to index it, doubling
userspace parsing work and adding background I/O contention right when `T=8`
benchmarks try to keep apply workers busy.

## Trigger

Run any write-enabled apply-load benchmark long enough for live-bucket spills to
produce file-backed outputs at levels 1+; the effect is strongest once those
outputs cross the 20 MB `DiskIndex` cutoff and especially in `T=8` scenarios,
where background merge rescans overlap with apply-thread reads and writes.

## Target Code

- `src/bucket/BucketOutputIterator.cpp:getBucket:168-249` — closes the freshly written bucket, then calls `createIndex()` when no reusable index exists
- `src/bucket/BucketIndexUtils.cpp:createIndex:30-51` — always constructs a new index from the bucket filename
- `src/bucket/LiveBucketIndex.cpp:LiveBucketIndex:41-69` — small file-backed live buckets build `InMemoryIndex(filename, ...)`, large ones build `DiskIndex`
- `src/bucket/InMemoryIndex.cpp:InMemoryIndex:119-158` — reopens the file and re-reads every entry for small file-backed live buckets
- `src/bucket/HotArchiveBucketIndex.cpp:HotArchiveBucketIndex:16-29` — always builds a `DiskIndex` from the output filename
- `src/bucket/DiskIndex.cpp:DiskIndex:132-299` — reopens the file, re-decodes each entry, rebuilds counters/type ranges/filter hashes, and may persist the index

## Evidence

The write path already sees entries in sorted order inside `BucketOutputIterator`
and already tracks the output byte offset (`mBytesPut`) while serializing them.
Despite that, `getBucket()` discards all of that structure, then `DiskIndex`
recreates `typeRanges`, `BucketEntryCounters`, `assetToPoolID`, and
`keyHashes` by reading the file back through `XDRInputFileStream::readOne()`.
For hot archive, this second pass happens for every non-reused output because
`HotArchiveBucketIndex` never has an in-memory alternative.

## Anti-Evidence

Level-0 live merges already bypass the file rescan when `mergeInMemory()` passes
`inMemoryState`, and hash dedup can also skip index creation if an identical
bucket already exists. The win therefore concentrates in file-backed merge
outputs and fresh hot-archive buckets, not the already-optimized level-0 path.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete file-backed merge path: `BucketBase::merge()` → `BucketOutputIterator` construction → `mergeInternal()` drives `putFunc` which calls `out.put(entry)` → `put()` writes each entry via `mOut.writeOne(*mBuf, &mHasher, &mBytesPut)` → after merge completes, `getBucket()` closes file, computes hash, falls through to `createIndex<BucketT>()` at line 233 → constructs `LiveBucketIndex(bm, filename, hash, ctx, hasher)` → for buckets ≥20MB builds `DiskIndex<LiveBucket>(bm, filename, pageSize, hash, ctx, hasher)` which reopens the just-written file and re-reads every entry via `XDRInputFileStream::readOne()` at line 169. Confirmed that every piece of data collected during this rescan (LedgerKey, file offset, entry type, pool parameters) is available during the original write pass in `put()`.

### Code Paths Examined

- `src/bucket/BucketBase.cpp:341-426` (`merge()`) — Creates `BucketOutputIterator`, drives `putFunc` lambda calling `out.put(entry)`, then calls `out.getBucket(bucketManager, &mk)`. No `inMemoryState` is passed for file-backed merges.
- `src/bucket/BucketOutputIterator.cpp:76-165` (`put()`) — Buffers one entry; on key change, writes buffered entry via `mOut.writeOne(*mBuf, &mHasher, &mBytesPut)`. The pre-write `mBytesPut` value is the file offset of the written entry. The full `BucketEntry` is available at write time.
- `src/bucket/BucketOutputIterator.cpp:168-250` (`getBucket()`) — Line 220: if no existing index, line 231-234 falls through to `createIndex()`. For file-backed merges, `inMemoryState` is always nullptr.
- `src/bucket/BucketIndexUtils.cpp:30-51` (`createIndex()`) — Constructs `BucketT::IndexT(bm, filename, hash, ctx, hasher)`, which dispatches to the filename-based constructor.
- `src/bucket/LiveBucketIndex.cpp:41-69` — Filename constructor: gets `pageSize` from config+filesize. If 0, builds `InMemoryIndex(bm, filename, hasher)`. Otherwise builds `DiskIndex<LiveBucket>(bm, filename, pageSize, hash, ctx, hasher)`.
- `src/bucket/DiskIndex.cpp:132-299` — **The rescan site**: opens `XDRInputFileStream` on the just-written file (line 153-154), reads every entry in a loop (line 169), extracting LedgerKey, computing SipHash24 for bloom filter, building `keysToOffset` range entries, updating `typeRanges`, `counters`, and `assetToPoolID`. Then builds `BinaryFuseFilter16` from collected hashes (lines 257-290). Optionally calls `saveToDisk()` (line 296-298).
- `src/bucket/InMemoryIndex.cpp:119-158` — Filename constructor: identical rescan pattern for small file-backed live buckets. Opens file, reads every entry, calls `processEntry()` which inserts into `InMemoryBucketState` (heap allocation per entry via `make_shared`).
- `src/bucket/HotArchiveBucketIndex.cpp:16-29` — Always constructs `DiskIndex` from filename; no in-memory alternative.
- `src/bucket/LiveBucket.cpp:423-464` (`fresh()`) — Also goes through `BucketOutputIterator::getBucket()` without `inMemoryState`, triggering the same rescan for file-backed fresh buckets.

### Findings

The inefficiency is confirmed and real. For every file-backed bucket produced by `merge()` or `fresh()`:

1. **Write pass**: `BucketOutputIterator::put()` writes each entry, tracking `mBytesPut` (the running byte offset) and the full `BucketEntry`. All data needed for index construction is available here: the `LedgerKey` (via `getBucketLedgerKey`), the file offset, the entry type, and pool parameters.

2. **Rescan pass**: After the file is closed, `createIndex()` reopens it and performs a complete sequential read + XDR decode of every entry. For `DiskIndex` (lines 169-250), this rebuilds `keysToOffset`, `keyHashes`, `typeRanges`, `counters`, and `assetToPoolID` — all of which could have been accumulated during the write pass.

3. **I/O cost per merge**: For an output bucket of size N_out, the rescan adds N_out bytes of sequential file I/O plus full XDR decode of every entry. Total merge output I/O goes from N_out (write) to 2×N_out (write + rescan), a 50% increase in output-side I/O.

4. **Feasibility**: The `BinaryFuseFilter16` constructor requires all key hashes at once (it's a static filter), but these can be collected in a vector during writes and used at `getBucket()` time. The page size is determined by config (`BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT`), not by file contents, so range index entries can be built incrementally. The `DiskIndex::Data` struct could be populated during writes and moved into the index at construction time.

**Severity downgraded from Medium to Informational because:**

- The rescan runs on **background worker threads** (inside `FutureBucket::startMerge()` tasks), not on the main ledger-close thread. It only affects benchmark timings through I/O contention.
- The merge itself reads two input buckets (total size ≥ output size), so the rescan's I/O is roughly 25-33% of total merge I/O, not 50% of the total.
- Standard benchmarks (200 ledgers) produce limited numbers of large merge outputs. Level 1 spills every 4 ledgers but produces small buckets. Only levels 3+ produce buckets likely to exceed the 20MB `DiskIndex` cutoff, and those spill infrequently (every 64+ ledgers).
- Hash dedup in `getBucket()` (line 214-218) can skip index construction entirely if an identical bucket already exists, reducing the effective frequency.
- This is complementary to but distinct from H004 (defer disk index persistence), which addresses only the `saveToDisk()` fsync at the end. H001 addresses the larger re-read that precedes `saveToDisk()`.

### PoC Guidance

- **Target code**: `src/bucket/BucketOutputIterator.h` and `src/bucket/BucketOutputIterator.cpp` — add state to accumulate index metadata during `put()`. `src/bucket/DiskIndex.h` and `src/bucket/DiskIndex.cpp` — add a constructor that takes pre-built `DiskIndex::Data` instead of a filename. `src/bucket/LiveBucketIndex.cpp` and `src/bucket/HotArchiveBucketIndex.cpp` — add constructors accepting pre-built data.
- **Change description**: In `BucketOutputIterator`, add members to track: `keyHashes` vector, `keysToOffset` range entries, `typeStartOffsets`/`typeEndOffsets`/`lastTypeSeen`, `counters`, `assetToPoolID`. At each actual write in `put()` (when `mOut.writeOne()` is called), record the pre-write `mBytesPut` as the entry's file offset and process the entry for index metadata. In `getBucket()`, construct the `DiskIndex` or `InMemoryIndex` from the accumulated data (building `BinaryFuseFilter16` from collected hashes) instead of calling `createIndex()` from filename. The page size can be determined from `mBytesPut` (final file size) and config after the write pass completes.
- **Correctness check**: Existing bucket tests (`[bucket]` and `[bucketindex]` tags) cover merge outputs and index correctness. The inline-built index must produce identical `keysToOffset`, `counters`, `typeRanges`, and `assetToPoolID` as the file-rescan path. The `DiskIndex::operator==` test helper can verify equivalence.
- **Benchmark focus**: Run apply-load benchmarks with `APPLY_LOAD_BL_SIMULATED_LEDGERS >= 1000` to ensure upper levels contain large enough buckets. Compare at T=8. Measure per-merge wall-clock time (via Tracy or merge timer) rather than overall ledger close time, since the improvement is in background threads. Expected improvement: 25-33% reduction in per-merge wall-clock time for large file-backed merges, but likely <5% improvement in overall benchmark metrics.
