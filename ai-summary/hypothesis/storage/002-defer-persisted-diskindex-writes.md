# H002: Defer persisted `DiskIndex` file writes out of the live apply path

**Date**: 2026-04-10
**Subsystem**: storage (bucket)
**Severity**: Low
**Impact**: background merge write-I/O reduction by removing restart-only work from benchmarked ledger close
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once a large bucket's `DiskIndex` has been constructed in memory and attached to
the live `Bucket` object, current-process lookups should proceed without waiting
for a serialized `.index` file to be durably written to disk. Persisting that
index should be allowed to happen later, because missing on-disk indexes can be
rebuilt on restart.

## Mechanism

For large buckets, `BucketOutputIterator::getBucket(...)` builds a `DiskIndex`
object and stores it in the newly adopted bucket. `DiskIndex` then immediately
calls `saveToDisk(...)`, which serializes the full index payload to a temp file,
fsyncs it on close, and durably renames it into the bucket directory. None of
that extra write traffic is needed for the current run: live queries go through
the in-memory `bucket->getIndex()` object, and startup/catchup already have code
to rebuild indexes when the file is absent or invalid.

This makes persisted index files effectively a restart-acceleration artifact
being produced on the hot apply path. Deferring `.index` persistence to an idle
task, a lower-priority maintenance thread, or shutdown would remove extra disk
writes and sync points from apply-load while preserving correctness of live
lookups.

## Trigger

Run long apply-load benchmarks with writes enabled until level-1+ bucket outputs
cross `BUCKETLIST_DB_INDEX_CUTOFF`. Every large merge then pays for bucket file
write + bucket index build + immediate serialized index-file write, even though
the live process already has the usable `DiskIndex` in memory.

## Target Code

- `src/bucket/BucketOutputIterator.cpp:getBucket:231-235` — creates a bucket index before adoption
- `src/bucket/LiveBucketIndex.cpp:51-68` — chooses `DiskIndex` when the bucket exceeds the 20 MB cutoff
- `src/bucket/DiskIndex.cpp:133-299` — constructs the in-memory `DiskIndex`
- `src/bucket/DiskIndex.cpp:296-299` — immediately persists the just-built index
- `src/bucket/DiskIndex.cpp:325-372` — serializes the index, fsyncs the file, and renames it into place
- `src/bucket/BucketListSnapshot.cpp:getBucketEntry:171-196` — current-process lookups use the attached in-memory index
- `src/catchup/IndexBucketsWork.cpp:70-106` and `src/bucket/BucketIndexUtils.cpp:55-85` — restart/catchup paths already tolerate missing or stale on-disk index files by rebuilding

## Evidence

The persisted `.index` file is not consulted by the live process after bucket
adoption; queries use the in-memory `IndexT` already attached to the bucket.
The code explicitly treats absent, corrupt, or out-of-date index files as
recoverable by rebuilding them later. That makes immediate persistence pure
extra work in the benchmark path, especially costly because it adds another full
file write plus durability barrier for every large merged bucket.

## Anti-Evidence

This optimization shifts cost from steady-state apply to restart/catchup, so it
trades faster live close times for slower warmup after a crash or restart unless
the deferred writer catches up. Some of the current cost is on background merge
threads, so end-to-end gains depend on how much those threads presently contend
with foreground apply and SQLite writes on the benchmark machine.
