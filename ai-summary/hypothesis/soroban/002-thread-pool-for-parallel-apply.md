# H002: Replace std::async Thread Spawning with Persistent Thread Pool

**Date**: 2026-04-09
**Subsystem**: soroban (ledger parallel apply)
**Severity**: Medium
**Impact**: Parallelization / latency / T=8 throughput
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

Parallel Soroban transaction application should use a pre-allocated thread pool
instead of spawning new OS threads for every cluster in every stage of every
ledger close. Thread creation and destruction overhead should not be part of the
critical apply path.

## Mechanism

`LedgerManagerImpl::applySorobanStageClustersInParallel()` (LedgerManagerImpl.cpp:
2427-2470) creates threads via `std::async(std::launch::async, ...)` for each
cluster in a stage. This spawns a fresh OS thread per cluster, which incurs:

1. **Thread creation overhead**: ~20-50µs per thread on Linux (kernel `clone()`,
   stack allocation, TLS setup). With 8 clusters per stage and multiple stages,
   this adds 160-400+µs per stage.
2. **Thread destruction overhead**: Each `std::future::get()` blocks until the
   thread completes and is joined. Thread teardown adds ~5-10µs per thread.
3. **Cache cold-start**: New threads start with cold CPU caches. A persistent
   pool would keep TLB and cache entries warm across stages and ledgers.
4. **No thread affinity**: `std::async` provides no control over CPU affinity,
   so threads may be scheduled on arbitrary cores, causing cache thrashing.

For the T=8 benchmark, if there are 2-4 stages with 8 clusters each, this is
16-32 thread creation/destruction cycles per ledger = ~320-1600µs of pure
overhead. Against a target ledger close of ~50-100ms, this is 0.3-3.2%.

More importantly, thread creation serializes the start of parallel work. All
clusters within a stage must first pay the thread creation cost before any actual
transaction work begins, adding latency to the critical path.

A persistent thread pool (e.g., a simple work-stealing or fixed-size pool) would
eliminate creation overhead, maintain cache warmth, and allow work to start
immediately when clusters are ready.

## Trigger

Run `scripts/run_apply_load_matrix.py` with T=8 scenarios. Profile
`applySorobanStageClustersInParallel`. If the hypothesis is correct, a
measurable fraction of the function's wall-clock time will be in
`std::async`/`pthread_create` and `std::future::get`/`pthread_join`, rather than
in `applyThread` execution.

Alternatively, add timing around the `std::async` calls and the `threadFuture.get()`
calls to measure the thread lifecycle overhead directly.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:applySorobanStageClustersInParallel:2427-2470` — spawns threads via `std::async(std::launch::async, ...)` per cluster per stage
- `src/ledger/LedgerManagerImpl.cpp:applyThread:2380-2417` — thread entry point, processes one cluster
- `src/ledger/LedgerManagerImpl.cpp:applySorobanStage:2517-2532` — calls `applySorobanStageClustersInParallel` for each stage
- `src/ledger/LedgerManagerImpl.cpp:applySorobanStages:2535-2553` — iterates stages, each spawning threads

## Evidence

1. `std::async(std::launch::async, ...)` is the only parallelism mechanism in the Soroban apply path. There is no thread pool infrastructure anywhere in the codebase for this purpose.
2. The pattern creates N threads per stage and destroys them all before moving to the next stage. If there are S stages, this is S×N thread lifecycles per ledger.
3. The benchmark configuration targets T=8, meaning up to 8 clusters per stage, so 8 thread creations per stage is typical.
4. Modern thread pool implementations (like `std::jthread` pools, `folly::CPUThreadPoolExecutor`, or a simple condition-variable-based pool) can dispatch work in <1µs per task vs. 20-50µs for `std::async`.
5. `SharedModuleCacheCompiler` (in `src/ledger/SharedModuleCacheCompiler.cpp`) already uses a producer-consumer thread pool pattern for Wasm compilation, demonstrating that the codebase is comfortable with persistent threads.

## Anti-Evidence

1. `std::async` with `std::launch::async` on modern Linux with glibc often reuses
   threads from a system-level thread cache, reducing actual creation overhead to
   ~5-10µs. The worst-case 50µs is for truly fresh threads.
2. Adding a thread pool introduces lifecycle management complexity: the pool must
   survive across ledger closes, handle cleanup on shutdown, and avoid holding
   references to stale state.
3. The benchmark may have a small number of stages (often just 1-2), limiting the
   multiplier effect.
4. The actual transaction execution time per cluster dwarfs the thread creation
   time, so the percentage improvement may be modest (likely 1-3% of total
   ledger close time, potentially more for ledgers with many stages).
