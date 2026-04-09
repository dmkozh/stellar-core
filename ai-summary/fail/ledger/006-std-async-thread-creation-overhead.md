# H006: Per-Stage Thread Creation via std::async Adds Overhead for Multi-Stage Parallel Apply

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Low
**Impact**: Thread pool reuse would reduce overhead in multi-stage T=8 scenarios
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When executing multiple parallel Soroban stages, thread resources should be reused across stages rather than creating and destroying threads for each stage independently. The `applySorobanStageClustersInParallel` function should maintain a thread pool that persists across stages.

## Mechanism

`applySorobanStageClustersInParallel()` (LedgerManagerImpl.cpp:2427-2471) uses `std::async(std::launch::async, ...)` to spawn one OS thread per cluster within each stage. For T=8 benchmarks with 8 clusters per stage, this creates 8 threads via `std::async`, waits for all to complete, destroys the futures, and then repeats for the next stage.

`std::async` with `std::launch::async` typically creates a new OS thread per invocation (implementation-dependent, but most standard libraries do this). Thread creation involves kernel syscalls, stack allocation (~8MB default on Linux), and TLS initialization. With multiple stages per ledger, the total overhead is `num_stages × num_clusters × thread_creation_cost`.

## Trigger

Run T=8 apply-load scenarios with transaction sets that produce multiple parallel stages. Profile thread creation/destruction using `perf` or Tracy. Measure the total time spent in `std::async` setup + `future.get()` teardown.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2446-2449` — `std::async(std::launch::async, ...)` creating threads per cluster
- `src/ledger/LedgerManagerImpl.cpp:2452-2468` — `future.get()` joining threads
- `src/ledger/LedgerManagerImpl.cpp:2547-2551` — Stage loop calling `applySorobanStage` repeatedly

## Evidence

1. `std::async(std::launch::async)` at line 2446 is the standard "create a real thread" API. Each call incurs ~10-50μs of thread creation overhead on Linux.
2. With 8 clusters, each stage creates 8 threads. For ledgers with multiple stages, this multiplies.
3. The pattern of creating threads per stage and immediately joining them is a well-known anti-pattern for latency-sensitive workloads. A persistent thread pool would amortize creation costs.

## Anti-Evidence

1. Most benchmark configurations produce a single stage (all transactions are independent), so the per-stage overhead is incurred only once per ledger: 8 threads × ~25μs = ~200μs. This is <0.1% of total ledger close time (~200-500ms).
2. `std::async` implementations on modern Linux may use thread caching internally, reducing actual kernel thread creation overhead.
3. The Soroban VM execution per thread dominates (~25-50ms per thread in SAC benchmark), making ~200μs of thread creation negligible.
4. A thread pool adds complexity (lifecycle management, error handling, work stealing) with minimal benefit for the single-stage common case.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The per-stage thread creation overhead (~200μs for 8 threads) is negligible compared to the Soroban VM execution time per thread (~25-50ms). Even with multiple stages, the total overhead would be ~0.5-1ms, well below the 5% threshold for Low severity. Most benchmark configurations produce a single stage, further reducing the impact. The optimization would add significant complexity (thread pool management, lifecycle) for <0.2% improvement.

### Lesson Learned

Thread creation overhead from `std::async` is a common concern but rarely a real bottleneck when thread work duration is >10ms. Only investigate thread pool patterns when per-invocation work is <1ms or thread creation frequency is >100/second.
