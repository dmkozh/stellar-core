# H003: Parallel apply reopens and re-decodes the same bucket files independently in every worker thread

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: Medium
**Impact**: parallel apply scalability
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Immutable bucket files should be read through shared backing objects in parallel apply, so eight worker threads touching the same buckets do not each pay their own `open`/`seek`/buffer-fill/XDR-page-decode overhead. Thread-local read position is necessary, but immutable bucket contents should still be shareable through `pread`, mmap, or page objects keyed by `(bucket, offset)`.

## Mechanism

`ThreadParallelApplyLedgerState` copy-constructs a fresh `ApplyLedgerStateSnapshot` per worker, and `SearchableBucketListSnapshot` intentionally resets `mStreams` on copy. As a result, the first lookup against a given bucket in each worker thread lazily opens a new `XDRInputFileStream`, and later page loads decode into that stream's private `mBuf`, so the same bucket page can be reopened and re-decoded independently by multiple threads in the same ledger.

## Trigger

Run the `sac`, `custom_token`, or `soroswap` benchmark at `T=8` with enough clusters that multiple workers consult the same mid/old-level buckets during validation or classic-entry loads. The effect is strongest once the working set no longer fits in level 0 and threads repeatedly hit the same shared bucket pages.

## Target Code

- `src/bucket/BucketListSnapshot.cpp:84-133` — snapshot copies reset `mStreams`; `getStream` lazily opens a file per snapshot copy
- `src/bucket/BucketListSnapshot.cpp:140-164` — page loads operate on the per-snapshot stream object
- `src/util/XDRStream.h:179-240` — `readPage` uses the stream's private mutable buffer and re-decodes the page contents
- `src/transactions/ParallelApplyUtils.cpp:610-623` — each worker thread copies `mLCLSnapshot` into its own thread state

## Evidence

The concurrency fix in the existing code is isolation by copy, not sharing: every copied snapshot gets fresh file caches, and every cache miss calls `stream->open(bucket->getFilename())`. That avoids races, but it also means T=8 can multiply the same bucket-file setup and page-decode work across workers even though the underlying bucket data is immutable.

## Anti-Evidence

The OS page cache will usually prevent repeated physical disk reads, so the win is mostly in syscalls, userspace copies, and duplicated XDR decoding rather than raw storage bandwidth. The fix also has to preserve thread safety, so a shared scheme likely needs `pread`/mmap-style reads rather than simply sharing the current seek-based stream objects.
