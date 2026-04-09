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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete success path from `doApply` through `collectEvents`,
`consumeRefundableResources`, and `finalizeSuccess`. Confirmed the inefficiency
is real: contract events are decoded from XDR bytes via `xdr_from_opaque` into
typed `ContractEvent` objects (line 735), and the return value is decoded into
`SCVal` (line 819), solely to compute `xdrSha256(success)` (line 821) for the
consensus-critical operation result hash. When metadata is disabled
(`mEnabled=false`), both `OpEventManager::setEvents()` (line 506) and
`OperationMetaBuilder::setSorobanReturnValue()` (line 457) are no-ops — the
decoded typed objects are immediately discarded. A raw-byte hash path is
mathematically equivalent and would eliminate all decode overhead.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:doApply:884-918` — Calls `collectEvents`, `consumeRefundableResources`, `finalizeSuccess` in sequence
- `src/transactions/InvokeHostFunctionOpFrame.cpp:collectEvents:706-754` — Iterates `out.contract_events`, tracks sizes via `buf.data.size()` (raw encoded size, no decode needed), then unconditionally decodes each event at line 735 (`xdr::xdr_from_opaque(buf.data, evt)`) into `success.events`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:finalizeSuccess:816-829` — Decodes return value at line 819, computes `xdrSha256(success)` at line 821 (hashes the struct by traversing the decoded typed objects), then calls `setEvents` (line 825) and `setSorobanReturnValue` (line 827) — both no-ops when meta disabled
- `src/transactions/EventManager.cpp:OpEventManager::setEvents:504-512` — Returns immediately when `!mEnabled`
- `src/transactions/TransactionMeta.cpp:OperationMetaBuilder::setSorobanReturnValue:455-463` — Returns immediately when `!mEnabled`
- `src/rust/src/soroban_proto_any.rs:490-516` — `result_value` is `res.encoded_invoke_result` (already XDR-encoded `SCVal`); `contract_events` is `res.encoded_contract_events` (already XDR-encoded individual `ContractEvent` objects)
- `src/protocol-curr/xdr/Stellar-ledger.x:526-530` — `InvokeHostFunctionSuccessPreImage` = `{SCVal returnValue; ContractEvent events<>;}` — struct hash is concatenation of field encodings
- `src/crypto/SHA.h:53-61` — `xdrSha256` streams XDR encoding into SHA256 via `xdr::archive` without allocation, but still traverses the typed object graph

### Findings

The inefficiency is real but the magnitude is small:

1. **Decode cost per event**: A typical `ContractEvent` (SAC transfer) is ~160-200 bytes of XDR. `xdr_from_opaque` must parse nested unions, allocate vectors for topics, and copy data — estimated ~0.5-1 µs per event.

2. **Events per transaction**: SAC transfers emit 1 event; soroswap emits 2-4 events. With `APPLY_LOAD_BATCH_SAC_COUNT=100`, batched SAC txs emit ~100 events per transaction.

3. **Return value decode**: Typically ~10-50 bytes (often `Void`), ~0.1 µs.

4. **Hash re-traversal**: `xdrSha256` traverses decoded objects to produce the hash stream. With raw bytes, this is replaced by a simple `SHA256::add()` memcpy — saves ~0.3-1 µs per event.

5. **Total per-transaction overhead (batched SAC, 100 events)**: ~100-200 µs for decode + ~50-100 µs for hash traversal vs. raw memcpy = ~150-300 µs saved per transaction. With host execution time of ~2000-5000 µs for 100-batch SAC, this is ~3-15% of per-tx time.

6. **Total per-transaction overhead (single SAC, 1 event)**: ~1-2 µs saved. Host execution ~200-500 µs. Improvement ~0.3-1%.

7. **Total per-transaction overhead (soroswap, 3-4 events)**: ~3-8 µs saved. Host execution ~500-2000 µs. Improvement ~0.2-1.5%.

**Key insight**: The batched SAC benchmark (`APPLY_LOAD_BATCH_SAC_COUNT=100`) amplifies this significantly because each *transaction* emits ~100 events (one per batched payment). This is the scenario where the optimization has the most impact.

The raw byte hash is provably equivalent: since `InvokeHostFunctionSuccessPreImage` is a flat XDR struct with `{returnValue, events<>}`, its hash equals `SHA256(encode(returnValue) || u32_be(count) || encode(event[0]) || ... || encode(event[N-1]))`. The Rust side already returns each component as valid XDR bytes.

**Correctness is preserved**: The hash remains identical (same bytes are hashed). The size tracking in `collectEvents` already uses `buf.data.size()` (raw encoded size), so it doesn't depend on decoding. `setEvents` and `setSorobanReturnValue` are no-ops when metadata is disabled. The `mProtocol23SACReconciliationEvents` handling in `setEvents` (line 772) runs AFTER the hash computation (line 821), so it doesn't affect the hash.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp` — `collectEvents` (lines 706-754) and `finalizeSuccess` (lines 816-829)
- **Change description**: When `mOpMeta` has meta disabled (check `!mOpMeta.isEnabled()` or pass a flag), modify `collectEvents` to skip the `xdr_from_opaque` decode (lines 734-736), and modify `finalizeSuccess` to compute the hash from raw bytes using `SHA256` directly:
  ```cpp
  // Raw byte hash path (when meta disabled):
  SHA256 hasher;
  hasher.add(ByteSlice(out.result_value.data.data(), out.result_value.data.size()));
  uint32_t eventCount = htonl(static_cast<uint32_t>(out.contract_events.size()));
  hasher.add(ByteSlice(reinterpret_cast<unsigned char const*>(&eventCount), 4));
  for (auto const& buf : out.contract_events) {
      hasher.add(ByteSlice(buf.data.data(), buf.data.size()));
  }
  mOpFrame.innerResult(mRes).success() = hasher.finish();
  ```
  When meta is enabled, keep the existing decode+hash+setEvents path unchanged.
- **Correctness check**: Test `"[soroban]"` and `InvokeHostFunctionTests` — verify the success hash matches for the same transactions with meta enabled vs. disabled. The hash MUST be identical in both paths.
- **Benchmark focus**: Run `apply-load` with batched SAC (`APPLY_LOAD_BATCH_SAC_COUNT=100`) and soroswap scenarios. The batched SAC scenario should show the most improvement (~3-15% per-tx improvement translating to ~2-8% overall) since it emits 100 events per transaction. Single-event scenarios (unbatched SAC) will show minimal improvement (<1%).
