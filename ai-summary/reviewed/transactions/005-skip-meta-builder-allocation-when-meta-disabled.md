# H005: Metadata-disabled bundles still allocate TransactionMeta builders

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Low
**Impact**: serial per-tx setup overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

With `METADATA_OUTPUT_STREAM=""` and non-test builds, creating `TxBundle` objects for a parallel Soroban stage should not allocate transaction-meta scaffolding. The metadata-disabled path should be close to storing only the tx pointer, result reference, and tx number.

## Mechanism

`applyTransactions` correctly sets `enableTxMeta` from `ledgerCloseMeta != nullptr`, and the apply-load benchmark template disables metadata output. But `applyParallelPhase` still constructs a `TxEffects` for every tx, and `TxEffects` immediately constructs a `TransactionMetaBuilder` that allocates protocol-specific op-meta vectors and `OperationMetaBuilder` objects even when `metaEnabled` is false. On `sac,TX=6400,...` this serial main-thread work repeats thousands of times per ledger before any worker thread starts.

## Trigger

Run `scripts/run_apply_load_matrix.py` with the stock benchmark config and profile `sac,TX=6400,T=1` or `sac,TX=6400,T=8`. Expect visible setup time in `applyParallelPhase`, `TxBundle` construction, and `TransactionMetaBuilder::TransactionMetaBuilder` despite metadata output being disabled.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:applyTransactions/applyParallelPhase:2641-2668,2710-2738` - computes `enableTxMeta=false` yet still constructs `TxBundle`/`TxEffects` for every tx
- `src/transactions/ParallelApplyStage.h:TxEffects::TxEffects and TxBundle::TxBundle:19-55,61-100` - unconditional `TransactionMetaBuilder` construction
- `src/transactions/TransactionMeta.cpp:TransactionMetaBuilder::TransactionMetaBuilder:924-974` - allocates op-meta vectors/builders even with `metaEnabled=false`
- `docs/apply-load-benchmark-sac.cfg:18-22` - benchmark disables metadata output

## Evidence

The constructor comment says a disabled meta builder should make dependent logic "very cheap", but the constructor still reserves `mOperationMetaBuilders`, resizes the protocol-specific operation-meta vector, and constructs `OperationMetaBuilder` objects for each operation. In the benchmark configuration those allocations are pure setup overhead because `ledgerCloseMeta` is never created.

## Anti-Evidence

This is a smaller opportunity than worker-thread hot-loop issues because each tx here has only one operation and the builder objects are short-lived. The likely impact is therefore lower and concentrated in high-tx-count scenarios such as the SAC benchmark.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full TxBundle construction path in `applyParallelPhase` (LedgerManagerImpl.cpp:2710-2766), through `TxEffects` constructor (ParallelApplyStage.h:22-26), into `TransactionMetaBuilder` constructor (TransactionMeta.cpp:924-974). Confirmed that when `metaEnabled=false`, the constructor still: (1) initializes a `TransactionMetaV4` XDR union via `TransactionMetaWrapper`, (2) emplaces and `resize(1)`s an `xdr::xvector<OperationMetaV2>` causing a heap allocation, (3) reserves and constructs one `OperationMetaBuilder` causing another heap allocation, (4) all wrapped in a `unique_ptr<TxEffects>` heap allocation. All downstream methods (`setLedgerChanges`, `setSorobanReturnValue`, `maybePushChanges`, `finalize`) correctly early-return when `!mEnabled`, so the XDR data is never read or written — it is pure waste.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:applyParallelPhase:2710-2766` — serial triple-nested loop constructing TxBundles for all 6400 txs before parallel work begins
- `src/transactions/ParallelApplyStage.h:TxBundle:64-70` — `new TxEffects(enableTxMeta, *tx, ...)` heap-allocates ~500+ bytes per tx
- `src/transactions/TransactionMeta.cpp:924-974` — constructor emplaces `xdr::xvector<OperationMetaV2>` and `resize(1)` even when `metaEnabled=false`, allocating heap for one default-constructed `OperationMetaV2` (ExtensionPoint + 2 empty xvectors)
- `src/transactions/TransactionMeta.cpp:477-505` — `OperationMetaBuilder` constructors store references/bools; `OpEventManager` constructor (EventManager.cpp:236-246) sets `mEnabled=false` cheaply
- `src/transactions/TransactionMeta.cpp:346-352,385-393,455-460` — all write methods early-return when `!mEnabled`, confirming XDR data is never touched
- `src/transactions/TransactionMeta.cpp:1036-1040` — `finalize()` asserts `mEnabled`, so it is never called on disabled builders

### Findings

The inefficiency is real: for 6400 Soroban txs with meta disabled, the setup loop performs ~19,200 unnecessary heap allocations (3 per tx: unique_ptr for TxEffects, xvector resize for OperationMetaV2, vector reserve for OperationMetaBuilders) plus ~3.2MB of XDR struct initialization that will never be read.

However, the actual wall-clock cost is small. At ~20-50ns per allocation, the total is ~0.4-1ms. The benchmark ledger close for `sac,TX=6400,T=8` is typically hundreds of milliseconds, making this <0.5% of total time. Cache pollution from the 3.2MB of dead allocations could have a secondary effect on the subsequent parallel phase, but this is speculative.

The fix is feasible but requires non-trivial refactoring: `OperationMetaBuilder` holds a `std::variant<reference_wrapper<OperationMeta>, reference_wrapper<OperationMetaV2>>` which requires a valid referent. When disabled, the builder still needs to exist (accessed via `getOperationMetaBuilderAt` during `parallelApply`), but the underlying XDR data does not. Options include: (a) use a static dummy `OperationMetaV2` for all disabled builders to reference, (b) change `mMeta` to a pointer that can be null, or (c) add a disabled-mode constructor that skips XDR allocation entirely.

Severity downgraded from Low to Informational: the estimated improvement is well below the 5% threshold for Low severity across all benchmark scenarios.

### PoC Guidance

- **Target code**: `src/transactions/TransactionMeta.cpp` — `TransactionMetaBuilder` constructor (lines 924-974); `src/transactions/TransactionMeta.h` — `OperationMetaBuilder` class (mMeta member)
- **Change description**: When `!metaEnabled`, skip `mOperationMetas` emplace/resize and `mOperationMetaBuilders` construction. Either (a) add a thread-local static `OperationMetaV2 dummy` for disabled builders to reference, or (b) convert `OperationMetaBuilder::mMeta` to `std::variant<..., std::monostate>` and guard accesses. The `TransactionMetaWrapper` initialization can also be skipped since all accessor methods check `mEnabled`.
- **Correctness check**: Existing parallel apply tests (`[tx]`, `[soroban]`) cover this path. Run `./src/stellar-core test --ll fatal -r simple --abort --disable-dots "[soroban]"` to verify correctness.
- **Benchmark focus**: Measure setup phase duration in `applyParallelPhase` before `applySorobanStages` on `sac,TX=6400,T=8`. Expected improvement: <1ms (~0.1-0.5% of total ledger close time). May not be visible above noise in current benchmarks.
