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

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — duplicate of soroban-env success #001 + soroban-env fails #003/#005/#014/#015/#016
**Failed At**: reviewer

### Trace Summary

This hypothesis is a composite of multiple already-investigated angles on the same code path. The dominant cost it identifies — per-TX cost-param XDR serialization and Rust-side deserialization — was already addressed by soroban-env success #001 (which added `const&` binding in C++ and the `get_or_deserialize_cost_params` Rust cache now visible at lines 797–817). The remaining sub-proposals (caching full `CxxLedgerInfo`, sharing cost-param cache across `shallow_clone`, eliminating byte-slice comparison, removing `ContractCostParams::clone()`) have each been individually investigated and rejected in soroban-env fails #003/#005/#014/#015/#016.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:95-123` — `getLedgerInfo` now uses `const&` for cost params (lines 112–113, fix from success #001), but still calls `toCxxBuf(cpu)` and `toCxxBuf(mem)` per TX. This residual serialization was investigated in soroban-env fail #003/#015: after success #001 eliminates the deep copy, remaining `toCxxBuf` overhead is ~50–100 ns/TX (two `xdr_to_opaque` calls on ~600 byte blobs).
- `src/transactions/InvokeHostFunctionOpFrame.cpp:608` — `getLedgerInfo()` called per-TX. The integer fields and network_id copy add ~20–50 ns. Full CxxLedgerInfo caching was investigated in soroban-env fail #003/#015 and produced benchmark regressions.
- `src/rust/src/soroban_proto_any.rs:797-817` — `get_or_deserialize_cost_params` cache hit path does `memcmp` on ~600 bytes (~10–20 ns) then `ContractCostParams::clone()` (~180–280 ns). The memcmp was investigated in soroban-env fail #014 (savings 0.04–0.09%); the clone in fail #016 (savings 0.14–0.21%, requires out-of-scope API change).
- `src/rust/src/soroban_proto_any.rs:787-794` — `shallow_clone` resets cached params. Investigated in soroban-env fail #005: one-time-per-thread deserialization amortized over hundreds of TXs yields ~24–64 µs/ledger-close (0.003–0.009%).

### Why It Failed

1. **Duplicate coverage.** Every component of this hypothesis has been previously investigated:
   - Cost-param serialization/deserialization → success #001 (already landed)
   - Full CxxLedgerInfo caching → fail #003/#015 (regressed benchmarks; remaining overhead ~50–100 ns/TX after success #001)
   - Sharing cache across shallow_clone → fail #005 (0.003–0.009% savings)
   - Byte-slice comparison optimization → fail #014 (0.04–0.09% savings)
   - ContractCostParams::clone() elimination → fail #016 (requires out-of-scope soroban-env-host API change)

2. **Residual costs are below noise floor.** After success #001, the aggregate remaining per-TX overhead from all components this hypothesis targets is: toCxxBuf (~100 ns) + integer fields + network_id copy (~50 ns) + memcmp (~15 ns) + clone (~230 ns) ≈ ~400 ns/TX. For 6400 SAC TXs: 6400 × 400 ns = 2.56 ms against ~750 ms close times = 0.34%. This is well below the 5% threshold for Low severity.

3. **CxxLedgerInfo caching already tried and regressed.** The soroban-env fail #003/#015 PoC attempted caching `CxxLedgerInfo` per-ledger and it regressed 5/6 benchmark scenarios, likely due to cache overhead exceeding the savings.

### Lesson Learned

This hypothesis aggregates multiple micro-optimizations that have each been individually investigated and found to be below the noise floor. Combining sub-threshold optimizations does not produce a super-threshold result when the aggregate savings (~0.34%) remains far below the 5% minimum. After success #001 eliminated the dominant cost on this path, further incremental improvements to the same code require fundamentally different approaches (e.g., changing the soroban-env-host Budget API to accept shared/cached params without cloning — which is out of scope).
