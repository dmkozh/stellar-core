# H004: Avoid Decoding Host Outputs When Benchmark Mode Disables Metadata

**Date**: 2026-04-09
**Subsystem**: soroban
**Severity**: Medium
**Impact**: CPU / allocation churn on Rust->C++ result handling
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When the apply-load benchmark runs with metadata output disabled and diagnostic
events disabled, the bridge should avoid eagerly decoding contract events and
return values unless some downstream consumer truly needs the typed XDR objects.
The success hash and resource accounting should be derivable from the raw encoded
output without paying a full decode cost on every successful invocation.

## Mechanism

The Rust bridge already returns `encoded_contract_events` and `result_value` as
opaque buffers, but C++ unconditionally parses every event and the return value
back into typed XDR during `collectEvents` and `finalizeSuccess`. In the
apply-load benchmark configuration (`METADATA_OUTPUT_STREAM = ""`,
`ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = false`) this parse work is still paid on the
hot path, so a lazy/raw hashing path or Rust-side preimage hashing could remove
decode work from every successful transfer/swap.

## Trigger

Run the benchmark scenarios from `scripts/run_apply_load_matrix.py` with the
benchmark config in `docs/apply-load-benchmark-sac.cfg`, especially
`custom_token` and `soroswap`, which emit contract events on every success. If
the hypothesis is correct, a profile will still show `xdr::xdr_from_opaque`
inside `collectEvents` / `finalizeSuccess` even though metadata streaming is
turned off.

## Target Code

- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:498-515` — returns contract events and return value as raw byte buffers
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::collectEvents:707-753` — eagerly decodes every returned contract event
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::finalizeSuccess:816-828` — eagerly decodes the return value before hashing / meta population
- `docs/apply-load-benchmark-sac.cfg:18-22` — benchmark disables Soroban metrics metadata stream/debug output

## Evidence

The bridge already transports outputs in encoded form, so the decode is a
follow-on decision in C++, not a requirement of the bridge itself. The benchmark
configuration explicitly disables metadata output, yet the success path still
materializes typed `ContractEvent` and `SCVal` objects before computing the
operation hash and passing data into `OperationMetaBuilder`.

## Anti-Evidence

Some typed event / return-value objects may still be needed for transaction
result hashing or internal meta structures, so the optimization probably needs a
new raw-byte hash path rather than simply skipping all decoding. If hash
construction from raw buffers is awkward or if downstream code implicitly
depends on populated `OperationMetaBuilder` state even with metadata output off,
the implementation cost may dilute the win.
