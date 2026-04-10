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
writing. The `DiskIndex` constructor’s second pass is not discovering new
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
