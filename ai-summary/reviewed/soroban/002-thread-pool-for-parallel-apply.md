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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete parallel apply path from `applySorobanStages` through `applySorobanStage` into `applySorobanStageClustersInParallel`. Confirmed that `std::async(std::launch::async, ...)` at line 2446 is the sole thread creation mechanism, creating one OS thread per cluster per stage. Each thread runs `applyThread` which processes a cluster of transactions. The `threadFuture.get()` calls at line 2457 block until each thread completes. The inefficiency is real but the overhead is substantially smaller than the hypothesis claims.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:applySorobanStageClustersInParallel:2427-2470` — Confirmed: sequential loop at lines 2441-2450 calls `std::async(std::launch::async, ...)` once per cluster. Each call incurs `clone()` syscall. A second loop at 2452-2468 calls `future.get()` to join threads.
- `src/ledger/LedgerManagerImpl.cpp:applyThread:2380-2417` — Thread entry point; iterates transactions in a cluster, calling `parallelApply` per tx. The per-cluster work is dominated by Soroban VM execution (typically 1-10ms+ per cluster), dwarfing thread creation overhead by 100-1000×.
- `src/ledger/LedgerManagerImpl.cpp:applySorobanStage:2517-2532` — Calls `applySorobanStageClustersInParallel` then `checkAllTxBundleInvariants` then `commitChangesFromThreads`. One stage = one thread creation/destruction cycle.
- `src/ledger/LedgerManagerImpl.cpp:applySorobanStages:2535-2553` — Iterates stages; metrics at line 2699-2702 track `maxClusters` and `stagesPerLedger`, confirming stages are typically 1-4.
- `src/ledger/SharedModuleCacheCompiler.cpp:start:138-195` — Hypothesis claims this is a "persistent thread pool." Incorrect: it creates threads via `std::thread` for a one-shot compilation job and joins them all in `~SharedModuleCacheCompiler`. Not a persistent pool pattern.

### Findings

**The inefficiency is real but the magnitude is dramatically overstated.**

1. **Thread creation cost**: The hypothesis claims 20-50µs per thread. On modern Linux with NPTL, `pthread_create` typically costs 3-10µs because glibc caches thread stacks via `madvise`. The `clone()` syscall itself is ~2-5µs. Measured benchmarks on modern x86-64 Linux consistently show 5-10µs for `std::async(std::launch::async)`.

2. **Actual overhead calculation**: With realistic costs of 5-10µs per thread creation and 1-3µs per join:
   - Per stage: 8 threads × (5-10µs create + 1-3µs join) = 48-104µs
   - With 2 stages (typical): 96-208µs total
   - Against 50-100ms ledger close: **0.1-0.4%**

3. **"Cache cold-start" argument is weak**: Each thread processes a different cluster with different contracts and storage entries. The working set changes significantly per invocation, so a persistent thread's L1/L2 cache would be largely evicted by new data anyway. L3 cache (shared across cores) provides fast refills at ~10-20ns per line.

4. **"Serialized start" argument is minimal**: The `std::async` calls are in a sequential loop, so thread N+1's creation waits for thread N's creation (but NOT for N's completion). With 8 threads at 5-10µs each, serial launch overhead is 35-70µs. Meanwhile, thread 1 is already executing transaction work while thread 8 is being created.

5. **SharedModuleCacheCompiler is NOT a persistent pool**: Hypothesis incorrectly cites this as evidence of "persistent threads." The compiler creates threads in `start()` and joins them all in its destructor — a one-shot batch pattern, not a reusable pool.

6. **No existing thread pool infrastructure**: The codebase has no reusable thread pool for the apply path. Implementing one requires non-trivial lifecycle management (surviving across ledger closes, state cleanup, shutdown coordination).

**Bottom line**: The inefficiency exists and the fix is technically sound, but the expected improvement of 0.1-0.4% of ledger close time is well below the noise floor of any benchmark. This would not produce a measurable result.

### PoC Guidance

- **Target code**: `src/ledger/LedgerManagerImpl.cpp:applySorobanStageClustersInParallel:2427-2470` — replace `std::async` loop with submission to a persistent thread pool
- **Change description**: Create a simple fixed-size thread pool (condition variable + work queue) that persists across ledger closes. The pool should have N worker threads (matching max cluster count). Replace the `std::async` loop with work submission and `std::future` collection. Pool lifetime should be tied to `LedgerManagerImpl` or `Application`.
- **Correctness check**: Existing parallel apply tests (`[soroban]` tag tests exercising parallel stages) should continue to pass. Key tests: any test calling `applySorobanStages` with multiple clusters.
- **Benchmark focus**: Measure wall-clock time of `applySorobanStageClustersInParallel` minus `applyThread` execution time (i.e., the thread lifecycle overhead). Expected improvement: ~50-200µs per ledger (<0.5% of total). Use `perf` or Tracy to compare `clone()`/`pthread_create` syscall counts before/after.
