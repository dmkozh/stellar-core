# H003: Memoize Ledger-Constant Bridge Metadata Instead Of Rebuilding It Per Tx

**Date**: 2026-04-10
**Subsystem**: crypto, rust
**Severity**: Medium
**Impact**: repeated ledger-config serialization and cost-parameter cache churn
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

All Soroban invocations within one ledger close should share a single immutable
bridge payload for ledger metadata: protocol version, sequence, timestamp,
network ID, and serialized CPU / memory cost parameters. Rust should not need
to re-parse, re-compare, or re-clone cost parameters on every host invocation
when the values are ledger-constant.

## Mechanism

`getLedgerInfo` rebuilds `CxxLedgerInfo` for every transaction and serializes
`cpu_cost_params` and `mem_cost_params` every time with `toCxxBuf`. On the Rust
side, `get_or_deserialize_cost_params` only caches *after* those bytes have
crossed FFI, and even its hit path still compares full serialized byte slices
and clones `ContractCostParams` for each call. This leaves a repeated per-tx
config-marshaling cost in the measured path even though the values are stable
for the whole ledger.

## Trigger

Run any apply-load scenario with many Soroban transactions and profile
`getLedgerInfo`, `toCxxBuf` for cost params, and
`ProtocolSpecificModuleCache::get_or_deserialize_cost_params`. Compare against a
build that caches a ready-to-pass `CxxLedgerInfo` (or at least the serialized
cost-param buffers) per apply phase / thread and reuses shared cost-param cache
state across module-cache handles.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:95-123` — `getLedgerInfo` serializes cost params and copies network ID on every call
- `src/transactions/InvokeHostFunctionOpFrame.cpp:601-610` — each host invocation passes a freshly built `CxxLedgerInfo`
- `src/rust/src/soroban_proto_any.rs:412-424` — cost params are reloaded from `ledger_info` on every invoke
- `src/rust/src/soroban_proto_any.rs:797-830` — Rust cache hit path still does byte-slice comparison and `ContractCostParams` cloning
- `src/ledger/LedgerManagerImpl.cpp:939-947` — apply callers obtain shallow-cloned module-cache handles
- `src/rust/src/soroban_proto_any.rs:787-794` — `shallow_clone` resets cached cost-parameter state on the cloned handle

## Evidence

The Rust code comments say the cache exists to avoid redundant per-TX XDR
round-trips, which confirms this path is expected to matter on hot workloads.
But the current design only caches the post-FFI deserialized form, while the
C++ side still rebuilds the serialized payload for every tx and cloned
module-cache handles lose any previously cached cost-parameter state.

## Anti-Evidence

This is an adjacent optimization to the already-landed cost-parameter cache, so
the remaining savings are smaller than the original deserialize-every-time path.
The cost-parameter blobs are also much smaller than full resource footprints, so
this should matter less than eliminating whole-resource or whole-output
serialization.
