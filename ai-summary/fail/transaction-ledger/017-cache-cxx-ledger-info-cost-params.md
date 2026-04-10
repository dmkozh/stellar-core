# H001: Cache Immutable `CxxLedgerInfo` Cost-Param Buffers Across Invoke-Host Calls

**Date**: 2026-04-10
**Subsystem**: transaction-ledger (transactions/InvokeHostFunctionOpFrame, soroban-env bridge)
**Severity**: Medium
**Impact**: C++↔Rust bridge setup overhead before every Soroban invoke
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The apply path should marshal the immutable ledger-info payload for a ledger
close once and reuse it across all invoke-host calls in that ledger, instead of
re-serializing the same cost-parameter XDR blobs for every transaction.

## Mechanism

`InvokeHostFunctionOpFrame` rebuilds `CxxLedgerInfo` for every host call, and
`getLedgerInfo` always serializes `cpuCostParams()` and `memCostParams()` with
`toCxxBuf`. Those `ContractCostParams` blobs are immutable across the whole
ledger close and are individually capped at 20kB, so the current code pays two
large XDR serializations and heap allocations per Soroban transaction even
though the Rust side now caches the deserialized form once it arrives.

## Trigger

Run `scripts/run_apply_load_matrix.py` for `custom_token` or `soroswap`,
especially `T=8`, and profile time/allocations under
`stellar::getLedgerInfo`, `xdr::xdr_to_opaque(ContractCostParams)`, and the cxx
bridge call boundary. The hypothesis is strongest when the workload executes
thousands of invoke-host calls against one unchanged Soroban network config.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:41-69` — rebuilds `CxxLedgerInfo` and serializes both cost-param vectors
- `src/transactions/InvokeHostFunctionOpFrame.cpp:974-981` — pre-v23 helper rebuilds ledger info per tx
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1161-1167` — parallel helper rebuilds the same ledger info per tx
- `src/transactions/InvokeHostFunctionOpFrame.cpp:544-553` — passes `getLedgerInfo()` into `rust_bridge::invoke_host_function`
- `src/rust/src/bridge.rs:193-207` — bridge takes `ledger_info: CxxLedgerInfo` by value
- `src/rust/src/soroban_proto_any.rs:412-423,797-815` — Rust now caches deserialized cost params, so the remaining repeated work is on the C++ serialization side
- `src/protocol-curr/xdr/Stellar-contract-config-setting.x:367-370` — each `ContractCostParams` blob may be up to 20kB

## Evidence

- `getLedgerInfo` calls `toCxxBuf(cpu)` and `toCxxBuf(mem)` unconditionally for every invoke-host call.
- The bridge payload is immutable for the whole ledger in apply-load: protocol version, ledger sequence, close time, network ID, base reserve, and Soroban cost params do not change between transactions.
- The recently added Rust cache removes repeated deserialization, which makes the repeated C++ XDR encoding/allocation stand out as the remaining redundant half of the round-trip.

## Anti-Evidence

- If the actual serialized cost-param payload is far smaller than the 20kB cap, the savings may land near the low end of the threshold.
- A cxx move may avoid one extra copy of the marshaled object, though it does not remove the `xdr_to_opaque` work itself.
- Heavier `soroswap` VM execution may dilute the benefit relative to lighter token-transfer scenarios.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — near-duplicate of confirmed success/soroban-env/001-cost-params-per-tx-xdr-roundtrip
**Failed At**: reviewer

### Trace Summary

Traced the complete `getLedgerInfo()` → `toCxxBuf` → Rust
`invoke_host_function_or_maybe_panic` → `get_or_deserialize_cost_params` path.
The C++ side at `InvokeHostFunctionOpFrame.cpp:58-62` already uses `const&`
bindings (eliminating deep struct copies), and the Rust side at
`soroban_proto_any.rs:797-817` already caches deserialized `ContractCostParams`
in `ProtocolSpecificModuleCache`. Both optimizations were confirmed and
published as `success/soroban-env/001-cost-params-per-tx-xdr-roundtrip` with
10-22% measured improvement across SAC and custom_token scenarios.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:41-69` — `getLedgerInfo()` constructs `CxxLedgerInfo`, binding `cpuCostParams()`/`memCostParams()` by `const&` (line 58-59), then calls `toCxxBuf` (line 61-62) which does `xdr_to_opaque` + heap allocation
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf<T>` calls `xdr_to_opaque(t)` and wraps in `make_unique<vector<uint8_t>>`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:551` — `getLedgerInfo()` called once per tx from `invokeHostFunction()`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:974-981` — pre-v23 `getLedgerInfo()` override reads from LedgerTxn header
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1161-1167` — parallel `getLedgerInfo()` override reads from `ParallelLedgerInfo`
- `src/rust/src/soroban_proto_any.rs:397-430` — Rust entry point: uses `ProtocolSpecificModuleCache` for cached deserialization on p23+
- `src/rust/src/soroban_proto_any.rs:797-817` — `get_or_deserialize_cost_params`: fast-path `RwLock` read, memcmp of serialized bytes, returns `cached_params.clone()` on hit
- `src/protocol-curr/xdr/Stellar-contract-config-setting.x:116-299` — `ContractCostType` has 86 variants; each `ContractCostParamEntry` is `{ext, constTerm, linearTerm}` = 20 bytes XDR → actual serialized size ~1.7kB per param vector, NOT the 20kB cap

### Why It Failed

This hypothesis targets the same root cause as the already-confirmed finding
`success/soroban-env/001-cost-params-per-tx-xdr-roundtrip`, which achieved
10-22% improvement by:

1. **C++ side**: Binding `cpuCostParams()`/`memCostParams()` by `const&` to
   eliminate deep struct copies before serialization (already deployed at
   lines 58-59)
2. **Rust side**: Caching deserialized `ContractCostParams` in
   `ProtocolSpecificModuleCache` to avoid per-tx XDR deserialization (already
   deployed at lines 797-831)

H001 proposes caching the **serialized** `CxxBuf` output to skip `toCxxBuf`
entirely. While the inefficiency is technically real, the remaining waste is
marginal:

- Each `ContractCostParams` serializes to ~1,724 bytes (86 entries × 20 bytes
  + 4-byte length prefix), not the ~20kB cap the hypothesis suggests
- Per-tx cost of 2× `xdr_to_opaque` + 2× heap alloc ≈ 1.5-2.2µs
- For SAC 3200 txs: ~5-7ms out of ~750ms close time (post-optimization) ≈ 0.7-0.9%
- Even for the lightest scenario, this is well below the 5% Low threshold

The bulk of the round-trip cost was in struct copying (C++) and XDR
deserialization (Rust), both of which are already addressed. The remaining
C++ serialization is a small fraction of an already-optimized path.

### Lesson Learned

When a success finding already confirms an optimization in the same code path,
check whether the proposed hypothesis targets a different layer of the same
problem. The Rust-side deserialization cache and C++ const-ref binding captured
the dominant costs; the remaining C++ XDR serialization overhead (~1.7kB per
param vector, 86 fixed entries) is too small to warrant a separate investigation.
Always check `success/` across ALL subsystems for findings targeting the same
source files, not just the hypothesis's own subsystem.
