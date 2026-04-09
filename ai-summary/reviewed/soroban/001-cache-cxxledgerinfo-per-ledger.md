# H001: Cache CxxLedgerInfo Cost-Param Serialization Once Per Ledger

**Date**: 2026-04-09
**Subsystem**: soroban
**Severity**: Low
**Impact**: CPU / allocation churn in per-TX bridge setup
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

The `CxxLedgerInfo` struct passed to `rust_bridge::invoke_host_function()` should
be constructed once per ledger (or once per thread) and reused across all
transactions, since every field — including the two serialized
`ContractCostParams` buffers — is identical for all transactions in a ledger.

## Mechanism

`getLedgerInfo()` (InvokeHostFunctionOpFrame.cpp:41-70) is called once per
transaction via `invokeHostFunction()` at line 551. Each call serializes both
`cpuCostParams` and `memCostParams` via `toCxxBuf()`, which allocates a new
`std::vector<uint8_t>` and calls `xdr::xdr_to_opaque()` for each. With ~86
`ContractCostParamEntry` items per params struct (each ~20 bytes XDR), the two
buffers total ~3.4KB of XDR serialization per call. It also copies the 32-byte
network ID byte-by-byte into a `rust::Vec<u8>` each time.

In a T=8 apply-load benchmark with 100+ Soroban transactions per ledger, this
produces ~340KB of redundant XDR serialization and ~200 vector allocations that
are structurally identical. While the Rust side already caches the *deserialized*
cost params (fail 001), the C++ side still re-serializes them every time.

The fix is straightforward: compute the `CxxLedgerInfo` once (either in the
`InvokeHostFunctionApplyHelper` constructor, the `ThreadParallelApplyLedgerState`,
or as a shared const per stage) and pass it by reference to `invokeHostFunction()`.
The `getLedgerInfo()` virtual method already exists as a hook — making it cache
the result on first call would be the minimal change.

Estimated savings: ~2-5µs per transaction (XDR serialization + allocation),
totaling ~200-500µs per 100-tx ledger. Against a ~50-100ms ledger close time,
this is ~0.2-1%. Closer to the Low threshold for the soroswap scenario which has
higher per-tx host execution times.

## Trigger

Run `scripts/run_apply_load_matrix.py` with `custom_token` or `soroswap` at T=8.
Profile `getLedgerInfo()` — specifically the `toCxxBuf(cpu)` and `toCxxBuf(mem)`
calls. If the hypothesis is correct, cumulative time in `xdr::xdr_to_opaque` for
cost params will be visible as a repeated cost across all transaction applies.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:41-70` — reconstructs CxxLedgerInfo with fresh XDR serialization per call
- `src/transactions/InvokeHostFunctionOpFrame.cpp:invokeHostFunction:551` — passes freshly constructed CxxLedgerInfo to bridge
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionParallelApplyHelper::getLedgerInfo:1161-1168` — parallel apply path, also re-serializes every time
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionPreV23ApplyHelper::getLedgerInfo:974-982` — pre-v23 path, also re-serializes

## Evidence

1. `getLedgerInfo()` is a virtual method called exactly once per transaction (line 551), and both overrides (lines 975, 1162) unconditionally call `stellar::getLedgerInfo()` which unconditionally calls `toCxxBuf()` on both cost params.
2. All inputs to `getLedgerInfo()` are constant within a ledger: sorobanConfig, ledgerVersion, ledgerSeq, baseReserve, closeTime, and networkID do not change between transactions.
3. The `ParallelLedgerInfo` struct already captures the scalar fields but NOT the cost params or network ID.
4. For parallel apply, each thread calls `getLedgerInfo()` independently, so N threads × M txs per cluster = N×M redundant serializations.

## Anti-Evidence

1. The Rust side already caches deserialized cost params (per fail 001), so the Rust deserialization cost is already amortized. The savings here are only the C++ serialization + allocation side.
2. For individual cost params serialization (~1.7KB), `xdr_to_opaque` is fast — likely 1-2µs each. The total per-tx overhead (~4µs) is small relative to host execution (~200-2000µs).
3. The `CxxLedgerInfo` struct is moved (not copied) into the bridge call, so making it shared/cached would require either cloning or changing the bridge to take it by reference. The CXX bridge struct would need to be passed by const-ref rather than by value.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (fail 001 covers Rust-side deserialization caching; fail 003 covers per-tx inputs; this covers C++-side per-ledger serialization)

### Trace Summary

Traced the full `CxxLedgerInfo` construction and consumption path. The free function `getLedgerInfo()` (line 41) calls `toCxxBuf()` on both `cpuCostParams` and `memCostParams` from `SorobanNetworkConfig`, each performing `xdr::xdr_to_opaque()` with a heap allocation. This function is called once per transaction from both the pre-v23 path (line 975) and the parallel apply path (line 1162). The bridge takes `CxxLedgerInfo` by value (`bridge.rs:202`), consuming it each time, but `soroban_invoke.rs` immediately passes it by reference (`&ledger_info`) to the protocol-specific handler. The Rust-side cost-param cache (`get_or_deserialize_cost_params`, line 797) does a byte comparison of the serialized buffers and returns cached deserialized params on match — confirming the Rust deserialization is already amortized. The only remaining waste is the C++ XDR serialization and allocation.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:41-70` — confirmed: calls `toCxxBuf(cpu)` and `toCxxBuf(mem)` unconditionally, plus byte-by-byte network_id copy
- `src/transactions/InvokeHostFunctionOpFrame.cpp:invokeHostFunction:544-553` — confirmed: `getLedgerInfo()` called once per tx, result passed by value to bridge
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionPreV23ApplyHelper::getLedgerInfo:974-982` — confirmed: delegates to free function, no caching
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionParallelApplyHelper::getLedgerInfo:1161-1168` — confirmed: delegates to free function, no caching
- `src/transactions/TransactionUtils.h:toCxxBuf:372-376` — confirmed: always allocates `make_unique<vector<uint8_t>>(xdr::xdr_to_opaque(t))`
- `src/rust/src/bridge.rs:193-208` — confirmed: bridge takes `ledger_info: CxxLedgerInfo` by value
- `src/rust/src/soroban_invoke.rs:7-32` — confirmed: receives by value, passes `&ledger_info` by reference to protocol handler
- `src/rust/src/soroban_proto_any.rs:415-424` — confirmed: Rust-side cache hit path compares serialized bytes, returns cached deserialized params
- `src/rust/src/soroban_proto_any.rs:797-817` — confirmed: `get_or_deserialize_cost_params` uses `RwLock`-protected cache keyed by serialized byte equality

### Findings

The inefficiency is real but very small:

1. **Per-transaction C++ cost**: 2 × `xdr_to_opaque(ContractCostParams)` (~1.7 KB each) + 2 heap allocations + 32-byte network_id copy into `rust::Vec`. Estimated ~2-5µs total.
2. **Per-ledger aggregate (100 txs)**: ~200-500µs of redundant work.
3. **Relative to ledger close**: ~0.2-1% of a 50-100ms ledger close.
4. **Rust-side impact**: Negligible — the Rust cache does a fast byte comparison (~1.7 KB memcmp) and returns cached results. The full deserialization only happens on the first tx of each ledger.
5. **Bridge constraint**: `CxxLedgerInfo` is passed by value at the CXX bridge boundary. To cache it, the most practical approach is caching the serialized byte vectors on the C++ side and constructing new `CxxBuf` wrappers via memcpy (skipping `xdr_to_opaque`). Alternatively, change the bridge to take `&CxxLedgerInfo` (const reference), which CXX supports for shared structs.

The severity is downgraded from Low to **Informational** because the estimated improvement (~0.2-1%) falls well below the Low threshold (5-10%). The optimization is correct and clean but would not produce measurable benchmark improvement.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp` — the `getLedgerInfo()` virtual method and its overrides
- **Change description**: Cache the serialized cost-param byte vectors (`vector<uint8_t>`) and network_id `rust::Vec<u8>` once per ledger (or per parallel apply stage). On subsequent calls, construct `CxxLedgerInfo` by copying the cached byte vectors into new `CxxBuf` wrappers (memcpy, not XDR serialization). The simplest approach: add `mCachedLedgerInfo` to `InvokeHostFunctionApplyHelper` and populate it on first `getLedgerInfo()` call, returning cloned copies on subsequent calls. For parallel apply, the cache could live on `ThreadParallelApplyLedgerState` to avoid cross-thread sharing.
- **Correctness check**: All existing Soroban tests (`[soroban]` tag) should pass unchanged, since the serialized bytes are identical.
- **Benchmark focus**: Profile `getLedgerInfo()` cumulative time in apply-load `soroswap` T=8. Expected improvement: ~200-500µs per 100-tx ledger (<1% of close time). This is unlikely to be visible in benchmark noise.
