# H004: Hoist CxxLedgerInfo Construction Out of Per-TX Path Into Thread State

**Date**: 2026-04-09
**Subsystem**: soroban
**Severity**: Low
**Impact**: CPU / allocation reduction in parallel apply hot path
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

In the parallel apply path, the `CxxLedgerInfo` struct (which includes the
serialized cost params and network ID) should be constructed once per thread
state and shared across all transactions in that thread's cluster, rather than
being reconstructed for every transaction.

## Mechanism

In the parallel apply path, `InvokeHostFunctionParallelApplyHelper::getLedgerInfo()`
(line 1162) calls `stellar::getLedgerInfo()` which constructs a fresh
`CxxLedgerInfo` including two `toCxxBuf()` serializations of cost params
(~1.7KB each) and a byte-by-byte copy of the network ID (32 bytes).

The `ThreadParallelApplyLedgerState` already holds `mSorobanConfig` and
`ParallelLedgerInfo` (which has version, seq, reserve, time, networkID).
Adding a pre-built `CxxLedgerInfo` to this state would eliminate per-TX work.

However, the CXX bridge takes `CxxLedgerInfo` by value in
`rust_bridge::invoke_host_function()`, meaning each call would still need to
clone the struct. The optimization only helps if the bridge signature is changed
to accept `const CxxLedgerInfo&` or if the `CxxBuf` data member uses shared
ownership (e.g., `shared_ptr<vector<uint8_t>>` instead of
`unique_ptr<vector<uint8_t>>`).

This is essentially a more concrete and actionable version of H001, focused
specifically on the parallel apply path and the bridge signature change needed
to make it work.

## Trigger

Same as H001. Profile `getLedgerInfo()` calls in the parallel apply path.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionParallelApplyHelper::getLedgerInfo:1161-1168` — per-tx construction
- `src/transactions/ParallelApplyUtils.h:ThreadParallelApplyLedgerState:71-` — thread state, natural home for cached CxxLedgerInfo
- `src/rust/src/bridge.rs:CxxLedgerInfo:70-82` — bridge struct definition
- `src/rust/src/bridge.rs:invoke_host_function:198-215` — bridge function signature (takes CxxLedgerInfo by value)

## Evidence

All fields of `CxxLedgerInfo` are constant per-ledger. The parallel apply path
already has a `ThreadParallelApplyLedgerState` that holds all the scalar inputs.
Only the serialized `CxxBuf` cost params and `rust::Vec<u8>` network ID are
missing from the thread state.

## Anti-Evidence

1. This is structurally similar to H001 and may be considered a duplicate.
   The distinction is that H004 focuses on the parallel path and proposes
   a specific fix location (thread state) whereas H001 is more general.
2. Changing the CXX bridge to accept `CxxLedgerInfo` by reference requires
   modifying the Rust bridge definition, which has cross-cutting implications.
3. The per-tx cost (~4µs) is genuinely small relative to host execution.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated in fail/ or success/ (fail/001 covers Rust-side deserialization caching; fail/003 covers per-tx unique inputs; this covers C++-side per-ledger CxxLedgerInfo construction)

### Trace Summary

Traced the complete `CxxLedgerInfo` construction, bridge transit, and consumption path. The free function `getLedgerInfo()` (line 41-70) unconditionally calls `toCxxBuf()` on both `cpuCostParams` and `memCostParams`, each performing `xdr::xdr_to_opaque()` with a heap allocation (~1.7KB each). Both the parallel apply path (line 1162) and the pre-v23 path (line 975) delegate to this function with no caching. The CXX bridge (bridge.rs:202) takes `CxxLedgerInfo` by value, but `soroban_invoke.rs` immediately passes it by reference to the protocol handler, and the Rust cost-param cache only reads the serialized bytes for a memcmp comparison before returning cached deserialized params.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:41-70` — confirmed: calls `toCxxBuf(cpu)` and `toCxxBuf(mem)` unconditionally per invocation, plus byte-by-byte networkID copy
- `src/transactions/InvokeHostFunctionOpFrame.cpp:invokeHostFunction:544-553` — confirmed: `getLedgerInfo()` called once per tx, result passed by value to bridge
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionParallelApplyHelper::getLedgerInfo:1161-1168` — confirmed: delegates to free function, no caching
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionPreV23ApplyHelper::getLedgerInfo:974-982` — confirmed: delegates to free function, no caching
- `src/transactions/TransactionUtils.h:toCxxBuf:372-376` — confirmed: always allocates `make_unique<vector<uint8_t>>(xdr::xdr_to_opaque(t))`
- `src/rust/src/bridge.rs:193-208` — confirmed: bridge takes `ledger_info: CxxLedgerInfo` by value (move semantics)
- `src/rust/src/soroban_invoke.rs:7-32` — confirmed: receives by value, immediately passes `&ledger_info` by reference to protocol handler; testutils path also consumes by value
- `src/rust/src/soroban_proto_any.rs:310-340` — confirmed: protocol handler takes `&CxxLedgerInfo`
- `src/rust/src/soroban_proto_any.rs:797-817` — confirmed: `get_or_deserialize_cost_params` does byte-equality comparison of serialized buf, returns cached deserialized params on match

### Findings

The inefficiency is real but very small:

1. **Per-TX C++ cost**: 2 × `xdr_to_opaque(ContractCostParams)` (~1.7KB each, ~86 entries × ~20 bytes XDR) + 2 heap allocations (`make_unique<vector>`) + 32-byte network_id copy into `rust::Vec`. Estimated ~2-5µs total.
2. **Per-ledger aggregate (100 txs)**: ~200-500µs of redundant serialization and allocation.
3. **Relative to ledger close**: ~0.2-1% of a 50-100ms ledger close time.
4. **Bridge constraint confirmed**: `CxxLedgerInfo` is moved (not copied) at the CXX boundary. Since `CxxBuf` contains `UniquePtr<CxxVector<u8>>`, the struct cannot be trivially cloned. Two valid fix approaches:
   - (a) Change bridge.rs to `ledger_info: &CxxLedgerInfo` — CXX supports shared struct references; the Rust side already only reads it. Requires updating bridge.rs, soroban_invoke.rs, and the testutils path.
   - (b) Cache the serialized byte vectors on the C++ side and reconstruct `CxxBuf` wrappers via memcpy per call (skipping `xdr_to_opaque`). Avoids bridge changes but still has allocation per call.
5. **Approach (a) is cleanest**: The C++ caller constructs `CxxLedgerInfo` once on the thread state and passes `const CxxLedgerInfo&` to the bridge. Zero per-TX cost for CxxLedgerInfo construction.
6. **Cross-cutting concern**: The bridge change affects the generated `RustBridge.h`/`RustBridge.cpp`. The testutils path (`maybe_invoke_host_function_again_and_compare_outputs`) currently takes `CxxLedgerInfo` by value and would need updating.

Severity downgraded from Low to **Informational** because the estimated improvement (~0.2-1% of ledger close time) falls well below the Low threshold (5-10% benchmark improvement). The optimization is correct and clean but would not produce measurable benchmark improvement.

### PoC Guidance

- **Target code**: `src/rust/src/bridge.rs` (change `ledger_info: CxxLedgerInfo` to `ledger_info: &CxxLedgerInfo`), `src/rust/src/soroban_invoke.rs` (update function signature), `src/transactions/InvokeHostFunctionOpFrame.cpp` (cache `CxxLedgerInfo` on thread state or helper)
- **Change description**: (1) Change bridge.rs line 202 from `ledger_info: CxxLedgerInfo` to `ledger_info: &CxxLedgerInfo`. (2) Update `soroban_invoke.rs` to take `&CxxLedgerInfo`. (3) Update testutils path if present. (4) Regenerate RustBridge.h/cpp. (5) On C++ side, add `CxxLedgerInfo mCachedLedgerInfo` to `ThreadParallelApplyLedgerState`, populate once in constructor, pass `const&` to `invokeHostFunction()`.
- **Correctness check**: All `[soroban]` tests should pass unchanged. The serialized bytes are identical; only the lifetime/ownership changes.
- **Benchmark focus**: Profile cumulative `getLedgerInfo()` time in apply-load `soroswap` T=8. Expected: ~200-500µs saving per 100-tx ledger. Unlikely to be visible above benchmark noise (~1-2% variance).
