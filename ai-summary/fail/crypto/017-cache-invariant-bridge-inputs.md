# H002: Cache Invariant Soroban Bridge Inputs Before The Measured Apply

**Date**: 2026-04-10
**Subsystem**: crypto, rust
**Severity**: High
**Impact**: per-tx XDR serialization and heap allocation on the bridge input path
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once apply-load has generated and pre-validated its Soroban transactions, the
measured ledger-close path should reuse immutable serialized bridge inputs
already attached to each transaction rather than re-encoding them during every
host invocation. `hostFunction`, `SorobanResources`, source account, and auth
entries do not change between validation and apply for a given transaction.

## Mechanism

`InvokeHostFunctionApplyHelper::invokeHostFunction` rebuilds all of those
buffers on every apply call: it serializes `hostFunction`, `mResources`, and
`sourceID` with `xdr::xdr_to_opaque`, and it rebuilds `authBatch` entry-by-entry
before crossing the FFI boundary. Apply-load reuses the same `TransactionFrame`
objects after pre-validation, so this work could be computed once before the
benchmark timer starts and then reused during the measured close path.

## Trigger

Run `custom_token` or `soroswap` apply-load and sample `toCxxBuf`,
`xdr::xdr_to_opaque`, and `CxxBatchBufBuilder::append` under
`InvokeHostFunctionApplyHelper::invokeHostFunction`. Compare against a build
that caches transaction-invariant bridge inputs on `InvokeHostFunctionOpFrame`
or its parent transaction during generation / validation and only injects the
per-tx PRNG seed and live ledger-entry batches at apply time.

## Target Code

- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf` always allocates and materializes an XDR buffer
- `src/transactions/InvokeHostFunctionOpFrame.cpp:579-610` — every apply reserializes host function, resources, source account, and auth entries
- `src/transactions/InvokeHostFunctionOpFrame.cpp:582-587` — auth batch is rebuilt entry-by-entry for each invocation
- `src/simulation/ApplyLoad.cpp:2138-2148` — benchmark validates generated txs before measurement and keeps the same tx objects alive

## Evidence

The invariant bridge inputs are already fully parsed and owned by the
transaction object before apply begins, and apply-load explicitly performs a
pre-validation pass before the measured workload. There is no existing cache in
`InvokeHostFunctionOpFrame` for any of the serialized bridge payloads despite
their immutability across the tx lifecycle.

## Anti-Evidence

The per-tx PRNG seed and live footprint entry batches are genuinely apply-time
data and cannot be precomputed the same way. Small SAC transfers have much
smaller auth / resource payloads, so most of the benefit should show up in
`custom_token` and `soroswap`, not uniformly across every scenario.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — duplicate of soroban-env fail #010 (condensed in `ai-summary/fail/soroban-env/summary.md`)
**Failed At**: reviewer

### Trace Summary

Traced the complete bridge input path in `InvokeHostFunctionApplyHelper::invokeHostFunction` (lines 578–610). Confirmed three `toCxxBuf` calls serialize `hostFunction` (~200–500 bytes), `mResources` (~400–1500 bytes), and `sourceID` (~36 bytes) via `xdr::xdr_to_opaque`, each allocating a new `std::vector<uint8_t>`. The auth batch is built via `CxxBatchBufBuilder::append` (lines 582–587), serializing 1–5 entries into a contiguous buffer. Total per-TX bridge input preparation is ~0.5–8 µs depending on payload complexity (SAC: ~1 µs, soroswap: ~5–8 µs).

### Code Paths Examined

- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf<T>` calls `xdr::xdr_to_opaque(t)` which does size-pass + alloc + serialize-pass. Real but small cost per call.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:578-610` — `invokeHostFunction` builds auth batch, allocates PRNG seed buffer, and makes three `toCxxBuf` calls before the FFI invocation. All six inputs are constructed fresh each time.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:42-93` — `CxxBatchBufBuilder` serializes auth entries into a single contiguous buffer with a lengths vector, reducing per-entry allocations to 2 total.
- `src/simulation/ApplyLoad.cpp:1955-2004` — `benchmarkModelTxTpsSingleLedger` generates txs, then measures `closeLedger(txs)`. The `generateSacPayments`/`generateTokenTransfers` functions validate txs before measurement starts (lines 2136–2149), keeping the same `TransactionFrameBasePtr` objects alive.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1316-1335` — `doParallelApply` creates a new `InvokeHostFunctionParallelApplyHelper` per tx, which calls `invokeHostFunction` → re-serializes all bridge inputs.

### Why It Failed

1. **Duplicate of soroban-env fail #010.** That investigation examined the identical proposal: "Cache immutable invoke request XDR fields (`hostFunction`, `resources`, `sourceID`, auth entries) before timed apply." It concluded: "Each TX is applied exactly once — no repeated serialization exists to eliminate; shifting work before benchmark timer is not a true savings; aggregate <0.5% for all scenarios (per-TX unique data, O(1) serializations)."

2. **Per-TX unique data serialized O(1) times.** Unlike ledger-wide shared data (e.g., cost params, which are the same O(1) struct serialized O(N) times — see soroban-env success #001), each transaction's `hostFunction`, `resources`, `sourceID`, and `auth` are unique. They are each serialized exactly once during the single apply call. "Caching" them before the benchmark timer would only shift ~0.5–8 µs of work per tx from the measured phase to the generation phase — not eliminate it.

3. **Aggregate savings ceiling <0.5%.** For the heaviest scenario (soroswap, TX=1000): 1000 txs × ~8 µs/tx = 8 ms. Against typical ledger close times of 2–5 seconds, that is 0.2–0.4% — well below any measurable benchmark threshold. For SAC (TX=3200): 3200 × ~1 µs = 3.2 ms against ~1–2 second closes, ≈0.2%.

### Lesson Learned

This is the same meta-pattern identified in soroban-env fail summary: "All attempts to cache O(N) per-TX-unique data (request fields, footprint entries) are ceiling-bounded at <0.5% because each is serialized exactly once." Only ledger-wide shared state serialized redundantly per-TX can produce measurable wins from caching.
