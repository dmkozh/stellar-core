# H002: Parallelize `ThreadParallelApplyLedgerState` Bootstrap

**Date**: 2026-04-10
**Subsystem**: ledger, transactions
**Severity**: Medium
**Impact**: T=8 stage-start parallelization bottleneck
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When a parallel Soroban stage begins, worker cores should start useful work
with minimal serial setup on the primary apply thread. Per-cluster state
materialization should not be fully completed on the main apply thread before
any worker is launched.

## Mechanism

`applySorobanStageClustersInParallel` constructs each
`ThreadParallelApplyLedgerState` synchronously inside the stage-launch loop and
only then calls `std::async`. That constructor copies the apply snapshot into a
thread-local snapshot object, copies previously-restored entry tracking, and
scans every key in the cluster footprint through
`collectClusterFootprintEntriesFromGlobal` before any worker starts running.

This means the apply thread serializes cluster bootstrap `N` times at the start
of every stage while the worker pool is still idle. Moving bootstrap inside the
worker task, or otherwise constructing thread state in parallel, would let the
stage's wall time approach actual execution time instead of
bootstrap-plus-execution time.

## Trigger

Run `scripts/run_apply_load_matrix.py` on `sac` or `custom_token` at `T=8`.
Profile the time between entering
`applySorobanStageClustersInParallel` and the first substantial work inside
`applyThread`, and compare it with a variant that constructs thread state inside
the async task body.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:applySorobanStageClustersInParallel:2427-2470` — constructs thread state before launching each async worker
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::ThreadParallelApplyLedgerState:610-623` — per-cluster bootstrap work done serially today
- `src/transactions/ParallelApplyUtils.cpp:collectClusterFootprintEntriesFromGlobal:562-608` — scans cluster footprints and copies matching global entries
- `src/transactions/ParallelApplyUtils.h:ThreadParallelApplyLedgerState members:74-121` — documents per-thread snapshot and preloaded-entry state that are materialized at bootstrap time

## Evidence

- The constructor explicitly copies `global.mLCLSnapshot` into `mLCLSnapshot`,
  and the header comments state this copy exists to provide fresh per-thread
  bucket I/O caches.
- `collectClusterFootprintEntriesFromGlobal` walks both read-only and
  read-write footprint vectors and their TTL companions for every tx in the
  cluster before the worker is launched.
- The `std::async` call is after construction, so none of this bootstrap work
  overlaps across clusters.
- This cost scales with `stage.numClusters()` and total footprint size, exactly
  the dimensions that matter most in the T=8 benchmark runs.

## Anti-Evidence

- `ApplyLedgerStateSnapshot` is still a shallow wrapper over shared immutable
  state, so some of the copy cost may be modest in practice.
- If a workload produces only one small stage with tiny footprints, the serial
  bootstrap may be hard to measure.
- Moving constructor work into the worker task may complicate exception
  handling and scope-management invariants around `LedgerEntryScope`.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PARTIAL — the parallelization approach is novel, but the underlying bottleneck (serial bookend overhead in parallel apply) was comprehensively investigated in `ai-summary/fail/transactions/009-cluster-state-bookends-cap-scaling.md` (H003), which found through benchmarking that addressing bookend overhead does not produce measurable improvement.
**Failed At**: reviewer

### Trace Summary

Traced the full bootstrap path from `applySorobanStageClustersInParallel` (LedgerManagerImpl.cpp:2427-2470) through `ThreadParallelApplyLedgerState` constructor (ParallelApplyUtils.cpp:610-623) and `collectClusterFootprintEntriesFromGlobal` (ParallelApplyUtils.cpp:562-608). Confirmed the serial loop constructs each thread state before launching `std::async`. The dominant cost is `getTTLKey()` calling SHA-256 per soroban footprint key (~2000 calls per cluster for SAC), plus hash-map lookups in `globalEntryMap`. Total serial bootstrap: ~15-20ms for SAC TX=3200,T=8. However, a concrete code blocker and prior benchmark evidence make this approach non-viable.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:2427-2470` (`applySorobanStageClustersInParallel`) — Serial loop at lines 2441-2449 constructs `ThreadParallelApplyLedgerState` via `make_unique` then launches `std::async`. All bootstrap completes before any worker starts.
- `src/transactions/ParallelApplyUtils.cpp:610-623` (`ThreadParallelApplyLedgerState` constructor) — Copies `ApplyLedgerStateSnapshot` (line 614, involves `SearchableBucketListSnapshot` copy with empty stream cache), clones module cache via Rust FFI (line 617), copies restored entries (line 621), then calls `collectClusterFootprintEntriesFromGlobal` (line 622).
- `src/transactions/ParallelApplyUtils.cpp:562-608` (`collectClusterFootprintEntriesFromGlobal`) — **Thread assertion at line 567-568**: `releaseAssert(threadIsMain() || app.threadIsType(Application::ThreadType::APPLY))`. This explicitly forbids execution on worker threads. The function iterates every tx footprint key, calls `getTTLKey()` (SHA-256) for soroban keys at line 602, and looks up entries in the const `globalEntryMap`.
- `src/ledger/LedgerStateSnapshot.cpp:85-96` (`SearchableBucketListSnapshot` copy constructor) — Copies shared_ptrs (cheap), leaves `mStreams` empty. The per-copy cost is primarily atomic refcount increments on shared_ptrs.
- `src/ledger/LedgerEntryScope.cpp:444-457` (`scopeAdoptEntryOptFromImpl`) — Checks `scope.mActive == false` (the global scope must be deactivated). The `DeactivateScopeGuard` at LedgerManagerImpl.cpp:2439 satisfies this, but concurrent reads of the `mActive` bool from multiple worker threads would require `std::async`'s happens-before guarantee.
- `src/ledger/LedgerEntryScope.h:188-192` (`FOR_EACH_VALID_SCOPE_ADOPTION`) — Confirms `GlobalParApply → ThreadParApply` is a valid scope adoption. No blocker here.

### Why It Failed

**1. Concrete code blocker — thread assertion (line 567-568):** `collectClusterFootprintEntriesFromGlobal` contains `releaseAssert(threadIsMain() || app.threadIsType(Application::ThreadType::APPLY))`. Worker threads from `std::async` are neither the main thread nor the apply thread type, so moving the constructor into the async body would trigger this assertion. Relaxing it requires design-level discussion about the safety invariants it protects.

**2. Prior benchmark evidence shows bookend overhead is not material:** The closely related investigation H003 (`ai-summary/fail/transactions/009-cluster-state-bookends-cap-scaling.md`) comprehensively analyzed the same serial bookend bottleneck. H003 attempted to reduce the overhead by caching TTL key derivations and precomputing the RW key set. Despite eliminating ~51,000 SHA-256 invocations from the bookend path, benchmarks showed **regressions** rather than improvements:
- `sac,TX=3200,T=8`: p50 **−5.50%**, p95 **−6.21%**
- `custom_token,TX=1600,T=8`: p50 **−4.48%**, p95 **−5.80%**

While H002 proposes a different mechanism (parallelization vs. caching), the benchmark evidence suggests the bookend overhead is not the actual bottleneck in ledger close time. Parallelizing a non-bottleneck will not produce measurable improvement.

**3. Estimated impact is marginal even under ideal conditions:** Even with perfect parallelization (no contention), savings would be ~17ms out of ~200-500ms total ledger close = ~3.5-8.5%. In practice, cache contention on the shared `globalEntryMap`, atomic operations for `shared_ptr` copies across 8 threads, and Rust FFI contention for concurrent `shallow_clone()` calls would reduce effective savings well below this. Likely <3% net improvement — below the Informational threshold for actionability.

### Lesson Learned

When a prior investigation (H003) benchmarks a comprehensive optimization for the same bottleneck and finds no measurable improvement, alternative approaches to the same bottleneck should be treated with skepticism. The benchmark evidence suggests that the serial bookend phases, while visually prominent in code review, are not the actual throughput-limiting factor for T=8 parallel apply. Future optimization efforts for parallel apply scaling should focus on the worker execution phase itself or the post-worker merge phase rather than the pre-worker bootstrap.
