# H004: Stream `ApplyStage` Materialization Instead of Building the Full Parallel Phase Up Front

**Date**: 2026-04-10
**Subsystem**: ledger, transactions
**Severity**: Medium
**Impact**: serial pre-parallel CPU and allocation pressure before Soroban workers begin
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The apply thread should only materialize the bookkeeping required for the
current parallel stage, or at least avoid allocating a full `TxBundle` /
`TxEffects` graph for every Soroban transaction before any worker starts.
Stage execution should begin promptly, with post-apply bookkeeping stored in a
form that does not force the whole parallel phase to stay fully materialized.

## Mechanism

`applyParallelPhase` iterates every stage and every cluster, constructs a
`TxBundle` for every Soroban tx, and appends the resulting `ApplyStage`s into
`applyStages` before calling `applySorobanStages`. Each `TxBundle` heap-owns a
`TxEffects`, and `TxEffects` constructs a `TransactionMetaBuilder`, which
allocates operation-meta state up front. Because `processPostTxSetApply` later
walks the stored `applyStages`, this entire nested object graph stays alive
until after the whole parallel phase has finished.

Streaming one stage at a time, or flattening the retained post-apply data to a
minimal per-tx worklist keyed by tx index, would remove a large serial
allocation burst from the front of the parallel phase and reduce peak memory
pressure. The benchmark test build is particularly sensitive here because tx
meta is forcibly enabled even when metadata output is configured off.

## Trigger

Run `scripts/run_apply_load_matrix.py` in the test build and profile the time
inside `applyParallelPhase` before entering `applySorobanStages`, plus heap
allocation counts attributable to `TxBundle`, `TxEffects`, and
`TransactionMetaBuilder`.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:applyParallelPhase:2710-2766` — builds every `ApplyStage` before any stage is executed
- `src/transactions/ParallelApplyStage.h:TxEffects:22-59` — per-tx effect container allocated for the whole parallel phase
- `src/transactions/ParallelApplyStage.h:TxBundle:64-104` — heap-allocates `TxEffects` in the constructor
- `src/transactions/TransactionMeta.cpp:TransactionMetaBuilder::TransactionMetaBuilder:924-974` — eagerly allocates per-op meta builders and XDR buffers
- `src/ledger/LedgerManagerImpl.cpp:processPostTxSetApply:2828-2874` — current consumer that forces `applyStages` retention after execution
- `src/ledger/LedgerManagerImpl.cpp:2645-2650` — BUILD_TESTS forces tx meta enabled

## Evidence

- `applyParallelPhase` fully materializes `applyStages` and only then calls
  `applySorobanStages`, so none of this construction overlaps with worker
  execution.
- `TxBundle` uses `std::unique_ptr<TxEffects>` instead of embedding effects,
  guaranteeing an extra allocation per Soroban tx.
- `TransactionMetaBuilder` reserves and constructs per-operation meta structures
  at `TxBundle` creation time, not lazily when the worker needs them.
- The retained `applyStages` are only needed because `processPostTxSetApply`
  reuses the nested bundle structure later for refunds and result/meta emission.

## Anti-Evidence

- Soroban benchmark transactions usually have a single operation, so each
  individual `TransactionMetaBuilder` is not very large.
- Refactoring this to a streamed representation would touch both execution and
  post-apply ordering logic, making the change comparatively invasive.
- If profiling shows host execution still dominates overwhelmingly, the serial
  construction burst may land closer to the low end of measurable impact.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PARTIAL — the TxBundle/TxEffects allocation target is novel, but the underlying premise (serial pre-worker overhead limits parallel scaling) was comprehensively investigated and benchmarked in `ai-summary/fail/transactions/009-cluster-state-bookends-cap-scaling.md` (H003) and `ai-summary/fail/ledger/016-parallelize-cluster-state-bootstrap.md` (H002), both finding that bookend overhead is not a meaningful bottleneck.
**Failed At**: reviewer

### Trace Summary

Traced the full `applyParallelPhase` flow (LedgerManagerImpl.cpp:2710-2766), confirming it builds all `ApplyStage`/`TxBundle`/`TxEffects` objects before calling `applySorobanStages` at line 2762. However, per-tx construction cost is lightweight (~2-5μs each), the benchmark workload produces exactly one stage (making streaming pointless), `processPostTxSetApply` requires the full data so early deallocation is impossible, and prior benchmarks of larger bookend optimizations showed regressions rather than improvements.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:2710-2766` (`applyParallelPhase`) — Nested loops build all `TxBundle` objects for all stages, then calls `applySorobanStages`. Confirmed all allocation is serial before any worker starts.
- `src/transactions/ParallelApplyStage.h:64-101` (`TxBundle` constructor) — Does `new TxEffects(...)` (one heap allocation) plus stores shared_ptr, reference, and uint64_t. Lightweight.
- `src/transactions/ParallelApplyStage.h:19-56` (`TxEffects` constructor) — Creates `TransactionMetaBuilder` and default `LedgerTxnDelta`. No heavy allocation.
- `src/transactions/TransactionMeta.cpp:948-1013` (`TransactionMetaBuilder` constructor) — For meta-enabled builds: creates `TransactionMetaWrapper` (small XDR variant switch), `TxEventManager` (empty event vector), `DiagnosticEventManager` (empty buffer). Reserves and constructs 1 `OperationMetaBuilder` for each Soroban tx (single-op). The `OperationMetaV2` resize(1) creates one small default-constructed XDR struct with empty inner vectors.
- `src/transactions/TransactionMeta.cpp:501-514` (`OperationMetaBuilder` constructor) — Stores references and creates `OpEventManager` (empty). No allocations beyond the event manager.
- `src/ledger/LedgerManagerImpl.cpp:2535-2553` (`applySorobanStages`) — Iterates stages sequentially, calling `applySorobanStage` per stage. For the benchmark, there is exactly one stage.
- `src/ledger/LedgerManagerImpl.cpp:2828-2874` (`processPostTxSetApply`) — Iterates all `applyStages`, accessing `getTx()`, `getResPayload()`, `getEffects().getMeta().getTxEventManager()`, and `getEffects().getMeta()` for each tx. Requires the full `TxBundle`/`TxEffects` graph to remain alive.

### Why It Failed

**1. Benchmark produces exactly one stage — streaming provides zero benefit.** The apply-load benchmark generates workloads with a single maximally-parallel stage (confirmed by H003: "apply-load asserts there is exactly one maximally parallel stage"). Streaming "one stage at a time" is identical to the current behavior when there is only one stage. There is nothing to pipeline or defer.

**2. Per-tx TxBundle/TxEffects allocation cost is small.** Each `TxBundle` construction involves: 1 heap allocation for `TxEffects` (~100-300ns), 1 `TransactionMetaBuilder` with 1 `OperationMetaBuilder` (storing references, creating empty event vectors), and 1 default `LedgerTxnDelta`. For Soroban txs with a single operation, estimated per-tx cost is ~2-5μs. For SAC TX=3200: ~6-16ms total. At 200-500ms ledger close time, this is 1.2-8%, below Medium and borderline for Low.

**3. Prior benchmark evidence shows even larger bookend optimizations produce regressions.** H003 (`fail/transactions/009-cluster-state-bookends-cap-scaling.md`) optimized the serial bookend phases by caching ~51,000 SHA-256 invocations (estimated 30-50ms savings), which is 2-8× larger than the TxBundle allocation overhead targeted here. Despite eliminating significant crypto work, benchmarks showed regressions: sac,TX=3200,T=8 p50 −5.50%, custom_token,TX=1600,T=8 p50 −4.48%. If removing 30-50ms of serial overhead doesn't help, removing 6-16ms of allocation overhead will not either.

**4. `processPostTxSetApply` requires full TxBundle retention.** The post-apply phase (lines 2828-2874) accesses `getTx()`, `getResPayload()`, and the full `getMeta()` builder (including `TxEventManager`) for each transaction. The "flattening to a minimal worklist" approach would still need to retain all of this data — the only saveable component is the `LedgerTxnDelta` (which is empty after merge). Memory pressure reduction would be negligible.

**5. The hypothesis overstates `TransactionMetaBuilder` allocation cost.** The constructor (line 966-978) explicitly handles the meta-disabled case with a "lightweight disabled-mode constructor." In meta-enabled builds (BUILD_TESTS), it creates one `OperationMetaV2` (resize(1)) and one `OperationMetaBuilder` per tx — both are small structs with empty inner vectors. The claim of "eagerly allocating per-op meta builders and XDR buffers" is technically true but the cost is minimal for single-operation Soroban transactions.

### Lesson Learned

When the benchmark workload produces exactly one parallel stage, any optimization that relies on inter-stage pipelining or per-stage early deallocation provides zero benefit. Additionally, the prior rejection of H003 (which targeted 3-8× more serial overhead than this hypothesis) establishes a strong empirical ceiling: the serial pre-worker allocation burst is not the actual throughput bottleneck for parallel apply. Future optimization efforts should target the worker execution phase or post-worker commit/merge, not pre-worker object construction.
