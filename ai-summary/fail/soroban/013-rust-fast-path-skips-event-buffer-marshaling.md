# H001: Rust Fast Path Skips Event-Buffer Marshaling When Meta Is Disabled

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Medium
**Impact**: Rust->C++ bridge allocation churn / batched SAC throughput
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When transaction metadata and diagnostic output are disabled, a successful
Soroban invocation should cross the Rust/C++ bridge with only the data C++ still
needs on the hot path: the success hash, aggregate event/result byte counts,
modified ledger entries, and rent fee. It should not marshal per-event
`RustBuf`s back to C++ if those events will never be materialized into
transaction meta.

## Mechanism

`invoke_host_function_or_maybe_panic` always packages `result_value` and every
encoded contract event into `InvokeHostFunctionOutput`, and C++ then walks those
buffers in `collectEvents` / `finalizeSuccess` even when metadata is disabled.
The apply-load SAC benchmark uses `batch_transfer` with
`APPLY_LOAD_BATCH_SAC_COUNT = 100`, so one successful transaction can emit
roughly 100 transfer events; returning all of those event buffers over the FFI
boundary creates avoidable allocation and vector-iteration work before C++
immediately discards the typed objects.

## Trigger

Run the apply-load SAC benchmark with the stock config
(`docs/apply-load-benchmark-sac.cfg`), especially `T=1` and `T=8`. Profile the
successful invoke path and look for time in Rust-side `Vec<RustBuf>` creation
and C++ iteration over `out.contract_events` even though
`DISABLE_TX_META_FOR_TESTING = true` and `METADATA_OUTPUT_STREAM = ""`.

## Target Code

- `src/rust/src/bridge.rs:InvokeHostFunctionOutput:34-55` — bridge result shape always includes per-event `RustBuf`s
- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:488-516` — success path always returns `result_value` and `contract_events`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::collectEvents:707-754` — iterates every returned event buffer just to count bytes and populate the success preimage
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::finalizeSuccess:816-829` — consumes returned result/event buffers to finalize the success path
- `docs/apply-load-benchmark-sac.cfg:18-24,37,49-52` — benchmark disables tx meta and diagnostics while using batched SAC transfers
- `src/simulation/ApplyLoad.cpp:generateSacPayments:2069-2110` — benchmark routes SAC through batched `batch_transfer`

## Evidence

The Rust bridge already has the full encoded success payload in hand on the Rust
side: `res.encoded_invoke_result` and `res.encoded_contract_events`. In the
benchmark configuration C++ does not need typed meta output, only resource
accounting and the consensus-critical success hash. That makes the current
per-event `RustBuf` marshaling structurally redundant for the benchmarked path,
especially in batched SAC where the event count is intentionally amplified.

## Anti-Evidence

The Soroban host already allocates `encoded_contract_events` internally, so a
bridge-only fast path would not eliminate event creation inside
`soroban-env-host`; it would only stop those buffers from crossing into C++.
Meta-enabled production paths still need the current per-event output shape, so
the optimization needs a new explicit fast path rather than replacing the
general interface outright.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — duplicate of reviewed/soroban/004-skip-output-decode-when-metadata-is-off.md and reviewed/soroban/002-hash-success-preimage-from-raw-bytes.md
**Failed At**: reviewer

### Trace Summary

Traced the complete success path from `doApply` through `collectEvents` and `finalizeSuccess`. The hypothesis claims events can be skipped when meta is disabled, but events are **consensus-critical**: `collectEvents` builds the `InvokeHostFunctionSuccessPreImage` (which contains `{SCVal returnValue; ContractEvent events<>}`), and `finalizeSuccess` computes `xdrSha256(success)` at line 821, producing the hash stored in `mOpFrame.innerResult(mRes).success()`. This hash is part of the transaction result that all validators must agree on. Additionally, `collectEvents` enforces the `txMaxContractEventsSizeBytes` resource limit (lines 721-733), which is also consensus-critical. Events MUST cross the FFI boundary regardless of metadata settings.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:doApply:884-918` — Calls `collectEvents`, `consumeRefundableResources`, `finalizeSuccess` unconditionally on success
- `src/transactions/InvokeHostFunctionOpFrame.cpp:collectEvents:706-754` — Builds `InvokeHostFunctionSuccessPreImage.events` by decoding each event; also enforces `txMaxContractEventsSizeBytes` limit (consensus-critical resource check)
- `src/transactions/InvokeHostFunctionOpFrame.cpp:finalizeSuccess:816-829` — Hashes success preimage at line 821 via `xdrSha256(success)`, producing consensus-critical operation result hash
- `src/protocol-curr/xdr/Stellar-ledger.h:3929-3944` — `InvokeHostFunctionSuccessPreImage` struct contains `{SCVal returnValue; xdr::xvector<ContractEvent> events}` — events are part of the hashed preimage
- `src/rust/src/soroban_proto_any.rs:488-516` — Rust success path constructs `InvokeHostFunctionOutput` with `contract_events` and `result_value`

### Why It Failed

The hypothesis's core claim — that event buffers can be skipped across the FFI boundary when metadata is disabled — is incorrect. The events are NOT consumed solely for metadata output. They serve two consensus-critical purposes that execute regardless of metadata settings:

1. **Success preimage hashing**: All events are included in `InvokeHostFunctionSuccessPreImage`, which is hashed via `xdrSha256()` to produce the operation result. Skipping events would change the hash and break consensus.

2. **Resource limit enforcement**: `collectEvents` accumulates `mMetrics.mEmitEventByte` and checks it against `txMaxContractEventsSizeBytes`. Transactions exceeding this limit are failed with `INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED`. Skipping events would bypass this critical check.

Furthermore, this hypothesis is substantially a duplicate of two already-reviewed hypotheses:
- **H004** (reviewed/soroban/004-skip-output-decode-when-metadata-is-off.md) — same observation about decode overhead, found VIABLE at Informational severity (not Medium as H001 claims)
- **H002** (reviewed/soroban/002-hash-success-preimage-from-raw-bytes.md) — proposes the specific raw-byte hashing technique, also found VIABLE at Informational severity

The refined version of this idea (skip decoding, hash from raw bytes) has already been captured by those two hypotheses with proper severity assessment.

### Lesson Learned

Events in `InvokeHostFunctionOutput.contract_events` are not metadata — they are consensus-critical inputs to the success preimage hash and resource limit enforcement. Any optimization in this area must preserve event marshaling across the FFI boundary; only the C++-side XDR decode step can potentially be optimized (and this has already been captured in H002 and H004 at Informational severity).
