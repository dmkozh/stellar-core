# H008: Thread-local bucket snapshot stream caches duplicate file opens in parallel apply

**Date**: 2026-04-10
**Subsystem**: storage (bucket, ledger, transactions)
**Severity**: Low
**Impact**: parallel apply read-side file-open churn
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If multiple parallel-apply worker threads need to read the same bucket files
from the same immutable ledger snapshot, they should ideally avoid reopening
identical bucket files and rebuilding independent per-thread stream caches.
Shared immutable read handles or `pread`-style access would be preferable if
snapshot lookups were a major part of the benchmark.

## Mechanism

`ThreadParallelApplyLedgerState` stores its own `ApplyLedgerStateSnapshot`, and
the snapshot copy constructor intentionally clears `mStreams`, so each thread
opens bucket files lazily for itself in `getStream(...)`. In a read-heavy
parallel workload, this could multiply file-open and stream-cache setup work by
the number of worker threads.

## Trigger

Run a T=8 Soroban workload where many worker threads repeatedly miss both the
global entry map and `InMemorySorobanState`, forcing fallbacks to
`mLCLSnapshot.loadLiveEntry(...)` against the same newest buckets.

## Target Code

- `src/transactions/ParallelApplyUtils.h:74-77` — each thread keeps its own snapshot copy with fresh file caches
- `src/bucket/BucketListSnapshot.cpp:85-113` — snapshot copy constructor clears `mStreams`
- `src/bucket/BucketListSnapshot.cpp:getStream:121-130` — first lookup opens the bucket file for that snapshot copy
- `src/transactions/ParallelApplyUtils.cpp:getLiveEntryOpt:723-732` — thread fallbacks to `mLCLSnapshot.loadLiveEntry(...)`
- `src/transactions/ParallelApplyUtils.cpp:563-607` — footprint keys are preloaded from the global map before threads run
- `src/transactions/ParallelApplyUtils.cpp:333-384` — modified classic entries are also copied into the global map before parallel apply

## Evidence

The thread-local copy and empty `mStreams` cache are explicit in the code, so
the same bucket file can indeed be opened independently by multiple worker
threads. The fallback path from `getLiveEntryOpt(...)` to `mLCLSnapshot` is also
real for keys absent from thread/global maps.

## Anti-Evidence

The benchmark path rarely seems to reach this fallback often enough for the open
duplication to matter. Soroban keys use `InMemorySorobanState`, and classic keys
that matter to parallel apply are proactively copied into the global map before
threads start. That means the snapshot stream path is mostly a cold miss path,
not a dominant T=8 cost center.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The duplicate per-thread stream caches are real, but parallel apply is designed
to satisfy most hot lookups from `mThreadEntryMap`, `mGlobalEntryMap`, or
`InMemorySorobanState` before touching the bucket snapshot. On the benchmarked
Soroban workloads, that makes this path too cold to plausibly move ledger-close
time by 5% or more.

### Lesson Learned

For storage read-side hypotheses in parallel Soroban apply, first prove the code
path is exercised after the global/thread preload stages. Snapshot lookup code
that is architecturally elegant to optimize can still be irrelevant if the hot
workload usually resolves from the staged in-memory maps.
