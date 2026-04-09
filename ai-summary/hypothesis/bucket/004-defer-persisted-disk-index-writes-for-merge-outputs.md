# H004: Persisting every disk index eagerly adds cold write amplification that fights apply-path I/O

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: Medium
**Impact**: background merge write amplification and p99 close time
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Merge outputs should only pay the cost of durable on-disk index serialization when that persisted index is actually needed before the process exits or restarts. During apply-load, where the process keeps running and merge workers immediately hand the in-memory index to the adopted bucket, background merges should not also serialize, fsync, and durable-rename a second index artifact for every large merged bucket.

## Mechanism

Once a bucket crosses the disk-index cutoff, `BucketOutputIterator::getBucket` calls `createIndex`, and `DiskIndex` immediately persists itself via `saveToDisk`, which writes a temporary index file, fsyncs it on close, and durable-renames it into the bucket directory. That extra index file is cold redundancy during the benchmark: the newly adopted bucket already carries the live index in memory, but the worker still performs a second durable write path that competes with ledger-close reads and bucket-file writes.

## Trigger

Run a long enough apply-load benchmark for live-bucket merges to produce buckets above the 20 MB disk-index cutoff, especially at `T=8` where worker-thread merge I/O can overlap with apply-thread bucket reads. This should show up most clearly in higher-percentile ledgers once upper levels start spilling.

## Target Code

- `src/main/Config.cpp:176-179,201` — disk indexes are enabled by default and XDR fsync is enabled by default
- `src/bucket/LiveBucketIndex.cpp:28-39,51-69` — buckets at or above the cutoff switch to `DiskIndex`
- `src/bucket/BucketOutputIterator.cpp:220-235` — merge output eagerly creates an index before adoption
- `src/bucket/DiskIndex.cpp:325-372` — `saveToDisk` writes, fsyncs, and durable-renames a separate index file
- `src/bucket/FutureBucket.cpp:406-459` — this work runs inside background merge tasks while apply continues

## Evidence

The persisted index is not needed for the current process to use the merged bucket: `adoptFileAsBucket` stores the already-built index inside the returned `Bucket` object. Yet large merge outputs still perform a second file-creation path for the serialized index, including another durable rename in the same directory, so each big merge writes both the bucket payload and a separate durable index payload before the benchmark ever benefits from that persisted copy.

## Anti-Evidence

This optimization only matters once merged buckets are large enough to use `DiskIndex`; small buckets stay on `InMemoryIndex` and never call `saveToDisk`. Persisted indexes materially improve restart behavior, so the likely fix is lazy or deferred persistence rather than removing on-disk indexes entirely.
