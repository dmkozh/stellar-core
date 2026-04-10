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
