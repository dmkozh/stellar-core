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

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete success path from Rust host output through C++ bridge processing. Confirmed that `soroban_proto_any.rs` returns pre-encoded XDR bytes (`encoded_invoke_result` as `Vec<u8>`, `encoded_contract_events` as `Vec<Vec<u8>>`), which are wrapped as `RustBuf` and returned to C++. On the C++ side, `collectEvents()` decodes each event buffer via `xdr_from_opaque()` into `ContractEvent` objects and `finalizeSuccess()` decodes the return value, then `xdrSha256(success)` re-serializes the entire `InvokeHostFunctionSuccessPreImage` struct into a SHA256 hash. When metadata is disabled (`METADATA_OUTPUT_STREAM = ""`), both `OpEventManager::setEvents()` and `OperationMetaBuilder::setSorobanReturnValue()` early-return, making the decoded C++ objects pure waste.

### Code Paths Examined

- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs:478-501` — Rust host encodes result value via `metered_write_xdr` and events via `encode_contract_events`, producing canonical XDR bytes
- `src/rust/src/soroban_proto_any.rs:488-516` — Success path wraps encoded bytes as `RustBuf` in `InvokeHostFunctionOutput`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:706-753` — `collectEvents()` iterates all event buffers, validates sizes from raw byte lengths, then decodes each via `xdr_from_opaque(buf.data, evt)` and copies into `success.events` vector
- `src/transactions/InvokeHostFunctionOpFrame.cpp:816-829` — `finalizeSuccess()` decodes return value via `xdr_from_opaque`, computes `xdrSha256(success)`, then calls `setEvents()` (no-op when meta off) and `setSorobanReturnValue()` (no-op when meta off)
- `src/crypto/SHA.h:37-61` — `xdrSha256` uses `XDRSHA256` streaming hasher that walks the C++ object tree via `xdr::archive`, re-encoding each field to compute the hash without allocating a buffer
- `src/transactions/EventManager.cpp:503-512` — `OpEventManager::setEvents()` returns immediately when `!mEnabled`
- `src/transactions/TransactionMeta.cpp:454-462` — `setSorobanReturnValue()` returns immediately when `!mEnabled`
- `docs/apply-load-benchmark-sac.cfg:19-20` — Benchmark config sets `METADATA_OUTPUT_STREAM = ""` disabling metadata

### Findings

The optimization is real and correct. The `InvokeHostFunctionSuccessPreImage` XDR encoding is canonically:

```
XDR(returnValue: SCVal) || big_endian_u32(events.length) || XDR(events[0]) || ... || XDR(events[n-1])
```

Since both Rust's `stellar-xdr` crate and C++'s xdrpp implement canonical XDR encoding (RFC 4506), the raw bytes from Rust are byte-identical to what `xdrSha256(success)` re-produces after decoding and re-encoding. Therefore, hashing the raw bytes directly produces the same hash.

The validation in `collectEvents()` (checking `txMaxContractEventsSizeBytes`) uses only `buf.data.size()` (raw byte sizes), not decoded event content, so it can proceed without decoding.

There are two sub-optimizations:
1. **Always**: hash from raw bytes (avoids the re-encode via `xdr::archive` tree walk)
2. **When meta is off**: skip decoding events and return value entirely (avoids all `xdr_from_opaque` calls and C++ object allocation)

Both are valid. #2 provides the larger savings in benchmark mode.

**Severity downgraded to Low**: The per-event decode cost is estimated at ~300-500ns (3-5 heap allocations + XDR parsing) and re-encode at ~100-200ns per event. For SAC batch=100 (~100 events/tx, 3000 tx), total savings are ~120-210ms per ledger close out of ~3-5 second closes, or roughly 3-7%. For soroswap (~4-6 events/tx), savings are smaller. This likely lands in the Low (5-10%) range for the most event-heavy scenarios, not Medium. However, if profiling shows higher per-event decode costs due to heap fragmentation under load, Medium is not impossible.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp` — `collectEvents()` and `finalizeSuccess()` methods in `InvokeHostFunctionApplyHelper`
- **Change description**: Add a raw-byte hashing path in `finalizeSuccess()`:
  1. Create a `SHA256` hasher
  2. Feed `out.result_value.data` directly (the raw XDR bytes of the SCVal return value)
  3. Feed a 4-byte big-endian `uint32_t` event count
  4. Feed each `out.contract_events[i].data` directly
  5. Use the resulting hash as the `success` hash
  6. When metadata is disabled (check `mOpMeta.getEventManager().isEnabled()` or equivalent), skip the `xdr_from_opaque` decodes entirely — only run validation from raw sizes
  7. When metadata IS enabled, still decode events/return value for `setEvents()`/`setSorobanReturnValue()` but use the raw-byte hash
- **Correctness check**: `InvokeHostFunctionTests` (specifically any test that validates the success hash), and the `"[tx]"` test tag for transaction processing tests. Add a test that verifies raw-byte hash matches `xdrSha256` for a representative event set.
- **Benchmark focus**: Run the apply-load matrix with soroswap and SAC scenarios. The `sac,TX=6400,T=1` and `soroswap,TX=*,T=1` scenarios should show improvement. Metric: median and p99 ledger close time. Expected improvement: 3-8% for event-heavy scenarios.
