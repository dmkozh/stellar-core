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
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::ThreadParallelApplyLedgerState:593-607` — per-cluster bootstrap work done serially today
- `src/transactions/ParallelApplyUtils.cpp:collectClusterFootprintEntriesFromGlobal:545-591` — scans cluster footprints and copies matching global entries
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
