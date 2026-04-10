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
