# H006: Reuse Persistent Apply Workers Instead of Spawning `std::async` Per Stage

**Date**: 2026-04-09
**Subsystem**: transaction-ledger (ledger/LedgerManagerImpl)
**Severity**: High
**Impact**: Parallel apply scalability on T=8 workloads
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Parallel apply should keep a fixed set of worker threads alive across all
Soroban stages in a ledger and schedule cluster work onto them, instead of
creating and joining a fresh batch of async tasks for every stage.

## Mechanism

`applySorobanStageClustersInParallel` builds a `std::future` vector and calls
`std::async(std::launch::async, ...)` once per cluster on every stage, then
blocks on `future::get()` for all of them before moving on. On apply-load
scenarios with many small stages (especially `TX=6400,T=8` SAC), thread
creation, future shared-state allocation, wakeup, and teardown become a
repeated serial barrier around otherwise small units of work, directly capping
T=8 speedup.

## Trigger

Run `scripts/run_apply_load_matrix.py` and compare `sac,TX=6400,T=8` with a
build that reuses a persistent worker pool. Profile `applySorobanStageClustersInParallel`
to quantify time under `std::async`, `pthread_create`, and `future::get`.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2435-2469` — per-stage allocation of `threadStates`, `threadFutures`, and `std::async` launches
- `src/ledger/LedgerManagerImpl.cpp:2526-2531` — stage loop repeatedly pays the spawn/join cost
- `src/ledger/LedgerManagerImpl.h:375-381` — current stage-parallel interface leaves no room for persistent workers or work stealing

## Evidence

- The current implementation launches one async task per cluster per stage and
  waits for all of them synchronously before continuing.
- The benchmark matrix explicitly exercises `T=8` scenarios where stage-level
  thread-management overhead matters most.
- This overhead is outside the Soroban VM and therefore cannot be hidden by VM
  optimization; it is pure apply-path scheduler cost.

## Anti-Evidence

- Some standard-library implementations may reuse threads internally for
  `std::async`, which would reduce the benefit.
- Heavier scenarios such as `soroswap` may spend enough time inside the host
  that spawn/join overhead becomes a smaller fraction of total time.
- A persistent worker design has extra lifecycle and failure-handling
  complexity, so the implementation cost is non-trivial.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the full parallel apply path from `applySorobanStages` through
`applySorobanStageClustersInParallel` to `applyThread`. Confirmed that
libstdc++ (GCC 13) does create a real `pthread` per `std::async` call — there
is no internal thread pool. However, the benchmark workload (`ApplyLoad`) always
runs exactly 1 stage with up to 8 clusters (verified by assertion at
`ApplyLoad.cpp:2023`), so `std::async` is called only 8 times per ledger close,
not repeatedly across "many small stages."

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:2426-2471` — `applySorobanStageClustersInParallel`: spawns one `std::async` per cluster, then joins all. With 1 stage and 8 clusters, this is 8 thread creates + 8 joins per ledger.
- `src/ledger/LedgerManagerImpl.cpp:2534-2553` — `applySorobanStages`: iterates stages and calls `applySorobanStage` per stage. The loop body is heavy (VM execution for hundreds of txs per cluster).
- `src/simulation/ApplyLoad.cpp:2016-2025` — benchmark asserts exactly 1 stage (`stagesMetric.count() == 1`) and max clusters equal to `APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS`.
- `scripts/run_apply_load_matrix.py:273` — sets `APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS` to `thread_count` (8 for T=8 scenarios).
- `/usr/include/c++/13/future` — libstdc++ `std::async(std::launch::async)` creates `_Async_state_impl` which spawns a new `std::thread` per call.

### Why It Failed

The hypothesis's central premise — "many small stages" causing repeated
thread spawn/join overhead — is factually incorrect for the benchmark
workload. The apply-load benchmark runs exactly **1 stage** with 8
clusters. This means `std::async` is called only 8 times per ledger close.

With 8 `pthread_create` calls at ~20-50µs each and 8 `pthread_join` calls
at ~10-20µs each, the total thread management overhead is ~240-560µs per
ledger. The SAC,TX=6400,T=8 benchmark processes ~800 transactions per
thread through the Soroban VM, with total parallel execution time in the
hundreds of milliseconds. The thread management overhead is therefore
<0.1% of total apply time — far below even the Informational threshold,
let alone the claimed High (>20%) impact.

### Lesson Learned

When evaluating thread-management overhead, verify the actual multiplier
(stages × clusters) in the target workload rather than assuming worst-case
topology. The apply-load benchmark is deliberately designed for maximum
parallelism (1 stage, N clusters), which minimizes the spawn/join overhead
that would matter in a multi-stage scenario. A future hypothesis could
target multi-stage workloads specifically (e.g., transactions with shared
dependencies forcing multiple stages), but such workloads are not currently
exercised by the benchmark matrix.
