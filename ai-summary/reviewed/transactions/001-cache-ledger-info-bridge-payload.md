# H001: Cache the per-ledger Rust bridge payload instead of rebuilding `CxxLedgerInfo` per tx

**Date**: 2026-04-10
**Subsystem**: transactions
**Severity**: Low
**Impact**: C++↔Rust bridge setup / repeated cost-param serialization
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

During a single ledger close, Soroban host invocations should reuse one immutable bridge payload for ledger-global fields such as protocol version, sequence number, close time, network ID, and serialized CPU/memory cost parameters. The apply path should not XDR-encode the same cost-param objects and rebuild the same `network_id` vector once per transaction when those values are identical for every invoke in the ledger.

## Mechanism

`InvokeHostFunctionOpFrame` currently rebuilds `CxxLedgerInfo` on every host call, including `toCxxBuf(cpuCostParams)`, `toCxxBuf(memCostParams)`, and a byte-by-byte copy of the network ID. The Rust bridge also takes `ledger_info` by value, so every invoke pays for constructing and moving this per-ledger payload even though the benchmark closes one ledger with 1600-6400 Soroban transactions that all share the same values.

## Trigger

Run `scripts/run_apply_load_matrix.py` and profile any Soroban scenario, especially `sac,TX=6400,T=8` or `custom_token,TX=3000,T=8`. Expect measurable time under `stellar::getLedgerInfo`, `toCxxBuf(cpuCostParams)`, `toCxxBuf(memCostParams)`, and `network_id.emplace_back(...)` on every invoke-host call.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:41-69` — rebuilds `CxxLedgerInfo` and serializes cost params every time
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionPreV23ApplyHelper::getLedgerInfo:974-981` — per-call pre-v23 wrapper
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionParallelApplyHelper::getLedgerInfo:1161-1167` — per-call parallel wrapper
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::invokeHostFunction:544-553` — passes freshly built `ledger_info` into the Rust bridge
- `src/rust/src/bridge.rs:CxxLedgerInfo/invoke_host_function:70-82,193-208` — `ledger_info` crosses the bridge by value
- `src/rust/src/soroban_invoke.rs:invoke_host_function:7-24` — host dispatcher receives the value on every invoke

## Evidence

The helper-level `getLedgerInfo` function serializes `cpuCostParams` and `memCostParams` into new `CxxBuf`s and rebuilds `network_id` for every operation invocation. In apply-load, all model transactions in the measured ledger share the same `(protocol_version, sequence_number, timestamp, network_id, cost params)`, so this bridge payload is immutable for the whole run while still being rebuilt thousands of times.

## Anti-Evidence

`base_reserve`, `sequence_number`, and `closeTime` do legitimately change across ledgers, so the cache must be scoped to one ledger close or one `ParallelLedgerInfo`/helper lifetime rather than made process-global. Recent Rust-side cost-param caching already removes some downstream deserialization cost, so the remaining win is specifically on the C++ serialization and bridge-transfer side.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete `CxxLedgerInfo` construction and consumption path. The free function `stellar::getLedgerInfo()` (lines 41-69) is called once per Soroban tx via the virtual `getLedgerInfo()` override in both `InvokeHostFunctionPreV23ApplyHelper` (line 975) and `InvokeHostFunctionParallelApplyHelper` (line 1162). Each call performs two `toCxxBuf()` XDR serializations of `ContractCostParams` (86 entries each, ~1.7KB output) plus a byte-by-byte 32-byte network_id copy. The resulting struct is moved by value into `rust_bridge::invoke_host_function` (line 544-553). The Rust side already caches the *deserialized* cost params in `ProtocolSpecificModuleCache` (soroban_proto_any.rs:797-831) via byte-comparison, but the C++ side still re-serializes identically every time.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:41-69` — confirmed: constructs new `CxxLedgerInfo`, calls `toCxxBuf(cpuCostParams)` and `toCxxBuf(memCostParams)` (XDR serialization of 86-entry vectors), copies 32-byte network_id byte-by-byte
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper:283-314` — confirmed: helper is constructed per-tx with no caching of `CxxLedgerInfo`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:invokeHostFunction:544-553` — confirmed: `getLedgerInfo()` called inside `invokeHostFunction`, result passed by value to bridge
- `src/transactions/InvokeHostFunctionOpFrame.cpp:doApplyForSoroban:1244-1247` — confirmed: new helper per-tx
- `src/transactions/InvokeHostFunctionOpFrame.cpp:doParallelApply:1274-1278` — confirmed: new helper per-tx
- `src/transactions/TransactionUtils.h:toCxxBuf:370-376` — confirmed: `xdr::xdr_to_opaque(t)` allocates a new vector and serializes the full XDR each time
- `src/rust/src/bridge.rs:invoke_host_function:193-208` — confirmed: `ledger_info: CxxLedgerInfo` passed by value (ownership transfer)
- `src/rust/src/soroban_proto_any.rs:get_or_deserialize_cost_params:797-817` — confirmed: Rust-side cache compares serialized bytes via memcmp, avoids re-deserialization on cache hit, but C++ still pays serialization cost
- `src/rust/src/soroban_proto_any.rs:shallow_clone:787-795` — confirmed: parallel-apply threads get fresh empty cost param caches, so first invocation per thread deserializes fully

### Findings

The inefficiency is real and confirmed at every level:

1. **C++ XDR serialization cost**: Each `toCxxBuf(ContractCostParams)` call serializes 86 `ContractCostParamEntry` structs (ext + constTerm + linearTerm) into ~1.7KB. Two calls per tx = ~3.4KB of XDR serialization per invocation. At TX=3200, that's ~10.9MB of redundant XDR work per ledger close.

2. **Heap allocation cost**: Each `toCxxBuf` creates a `unique_ptr<vector<uint8_t>>` (heap alloc). Two per tx + one `rust::Vec` for network_id = ~3 heap allocations per tx = ~9600 avoidable heap allocations per ledger close at TX=3200.

3. **Rust-side cache overhead**: Even with the Rust-side cost param cache, every invocation still pays for a memcmp of ~1.7KB × 2 = ~3.4KB of byte comparison per tx. With cached `CxxLedgerInfo`, the same bytes would be compared (cache always hits), but the comparison is still performed.

4. **Bridge transfer cost**: `CxxLedgerInfo` is passed by value, transferring ownership of heap-allocated buffers from C++ to Rust on every invocation.

**Severity downgrade rationale**: The hypothesis claims Low (5-10%) improvement. However, the actual cost is estimated at ~5-10ms per ledger close (XDR serialization of ~10.9MB at ~1-2GB/s effective throughput plus heap allocation overhead), against total close times of 1-5 seconds for benchmark scenarios. This represents ~0.2-1% of total close time — meaningful for a micro-optimization but below the 5% threshold for Low severity. The Soroban host VM execution time dominates each invocation.

### PoC Guidance

- **Target code**: 
  - `src/transactions/InvokeHostFunctionOpFrame.cpp` — cache `CxxLedgerInfo` in `InvokeHostFunctionApplyHelper` constructor (or at a higher scope: per-ledger in the apply loop)
  - `src/rust/src/bridge.rs:202` — change `ledger_info: CxxLedgerInfo` to `ledger_info: &CxxLedgerInfo` in `invoke_host_function` signature
  - `src/rust/src/soroban_invoke.rs:16` — update to accept `&CxxLedgerInfo`
  - `src/rust/src/soroban_proto_any.rs:~395` — update `invoke_host_function_or_maybe_panic` parameter

- **Change description**: The cleanest approach is to change the bridge to pass `CxxLedgerInfo` by reference (`&CxxLedgerInfo`), then cache the struct in the `InvokeHostFunctionApplyHelper` base class constructor. For the pre-v23 path, build once from `LedgerTxn` header in the constructor. For the parallel path, build once from `ParallelLedgerInfo` in the constructor. The virtual `getLedgerInfo()` method can be replaced by a const member reference. The CXX bridge supports passing shared types by reference, so `&CxxLedgerInfo` is valid.

- **Correctness check**: Existing Soroban tests (`[soroban]` tag) cover `InvokeHostFunctionOpFrame::doApplyForSoroban` and `doParallelApply`. The `[tx]` tag tests cover the general apply path. Run `"[soroban]"` test suite to verify no regressions. Also verify that the `#[cfg(feature = "testutils")]` path in `soroban_invoke.rs:41-58` still compiles (it passes `ledger_info` by value to `maybe_invoke_host_function_again_and_compare_outputs`).

- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` with `sac,TX=3200,T=8` and `custom_token,TX=1600,T=8`. Expected improvement is small (Informational — likely <1% on median close time). Could be more visible under profiling (perf/Tracy) as reduced time in `stellar::getLedgerInfo` and `xdr::xdr_to_opaque`.
