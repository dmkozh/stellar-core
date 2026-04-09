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
