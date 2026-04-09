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
