# H002: Hash Success Preimage From Returned XDR and Skip C++ Decode When Meta Is Off

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Medium
**Impact**: CPU / output marshaling
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When transaction metadata output is disabled, the bridge should not deserialize
the returned `result_value` and `contract_events` into C++ XDR objects just to
hash them or immediately discard them. The success path should compute the
`InvokeHostFunctionSuccessPreImage` hash directly from the already-encoded Rust
buffers, and only decode return value / events when operation metadata is
actually enabled.

## Mechanism

Rust already returns `encoded_invoke_result` and `encoded_contract_events` as
raw XDR bytes in `InvokeHostFunctionOutput`. On the C++ side, `collectEvents()`
and `finalizeSuccess()` decode those bytes into `ContractEvent` / `SCVal`,
populate a transient `InvokeHostFunctionSuccessPreImage`, and then
`xdrSha256(success)` re-encodes the same structure. Under the benchmark config
(`METADATA_OUTPUT_STREAM = ""`), `OpEventManager::setEvents()` and
`OperationMetaBuilder::setSorobanReturnValue()` are disabled, so that decode +
re-encode loop is pure bridge overhead.

## Trigger

Run the standard apply-load benchmark config (`docs/apply-load-benchmark-sac.cfg`
and the matrix script) with any Soroban workload that emits contract events.
`soroswap` should be the strongest reproducer because it returns multiple events
per swap, but SAC and custom-token transfers should also exercise the path.

## Target Code

- `docs/apply-load-benchmark-sac.cfg:18-22` — benchmark disables metadata output
- `src/rust/src/bridge.rs:34-54` — `InvokeHostFunctionOutput` currently returns raw result/event buffers
- `src/rust/src/soroban_proto_any.rs:488-516` — success path assembles `RustBuf` vectors from already-encoded host output
- `src/transactions/InvokeHostFunctionOpFrame.cpp:707-753` — `collectEvents()` decodes every returned event
- `src/transactions/InvokeHostFunctionOpFrame.cpp:816-827` — `finalizeSuccess()` decodes return value and hashes a reconstructed preimage
- `src/transactions/EventManager.cpp:504-513` — `OpEventManager::setEvents()` becomes a no-op when meta is disabled
- `src/transactions/TransactionMeta.cpp:455-463` — `setSorobanReturnValue()` becomes a no-op when meta is disabled

## Evidence

The Rust bridge already has the exact XDR bytes needed for `returnValue` and
the `events<>` vector; no host-internal re-encoding is required to obtain them.
In benchmark mode, metadata is disabled, so C++ neither stores the decoded
events nor stores the decoded return value, yet it still pays to decode them
and then immediately re-encode them via `xdrSha256(success)`. That waste grows
with event count, which makes event-heavy `soroswap` transactions particularly
attractive.

## Anti-Evidence

The raw-byte hashing path must exactly match the canonical XDR layout of
`InvokeHostFunctionSuccessPreImage`, including the vector length prefix for
`events<>`, so the implementation is correctness-sensitive. If any benchmark or
production configuration enables metadata output, C++ still needs the decoded
events and return value for meta population, so this needs either a conditional
fast path or a bridge API that returns both a precomputed hash and the buffers.
