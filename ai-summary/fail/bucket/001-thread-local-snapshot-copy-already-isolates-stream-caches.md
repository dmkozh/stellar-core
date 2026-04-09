# H001: Shared snapshot stream-cache contention across apply threads

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: High
**Impact**: parallel apply scalability
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Parallel apply threads should read bucket files through independent snapshot stream caches so one thread's seeks do not interfere with another's. If stream caches were shared across threads, 8-thread apply would serialize or race on the same `XDRInputFileStream` objects.

## Mechanism

At first glance, `SearchableBucketListSnapshot` stores mutable `mStreams` without synchronization, and `ThreadParallelApplyLedgerState::getLiveEntryOpt` calls into the snapshot during parallel apply. That suggests multiple apply threads might contend on or race through one shared snapshot object.

## Trigger

Run an 8-thread apply-load scenario where multiple clusters perform classic bucket reads in parallel.

## Target Code

- `src/bucket/BucketListSnapshot.h:92-94,161-164` — per-snapshot mutable stream cache; copy constructor intentionally resets it
- `src/transactions/ParallelApplyUtils.h:74-76` — thread state comment says each thread gets a copied snapshot with fresh file caches
- `src/transactions/ParallelApplyUtils.cpp:610-618` — thread-state constructor copy-constructs `mLCLSnapshot` from the global snapshot

## Evidence

`SearchableBucketListSnapshot` explicitly documents that copies are safe to use from different threads because `mStreams` is reset on copy. `ThreadParallelApplyLedgerState` stores `mLCLSnapshot` by value and initializes it from `global.mLCLSnapshot`, so each apply thread receives its own snapshot object rather than sharing the global instance's file streams.

## Anti-Evidence

The global parallel state still holds a snapshot by value, which made the shared-cache interpretation initially plausible. But the thread-local copy happens before the snapshot is used in worker threads, and the header comment matches the implementation.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

Parallel apply already avoids shared stream caches by copy-constructing a fresh `ApplyLedgerStateSnapshot` inside each `ThreadParallelApplyLedgerState`.

### Lesson Learned

For bucket snapshot concurrency questions, check the copy path into `ThreadParallelApplyLedgerState` before assuming the global snapshot object is shared on worker threads. The stream-cache isolation is implemented via the snapshot copy constructor, not with locks around `mStreams`.
