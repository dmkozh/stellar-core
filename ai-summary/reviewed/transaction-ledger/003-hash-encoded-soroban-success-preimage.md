# H003: Hash the Encoded Soroban Success Preimage Before C++ Decodes It

**Date**: 2026-04-10
**Subsystem**: transaction-ledger
**Severity**: Low
**Impact**: Post-host bridge overhead after successful invoke-host execution
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The invoke-host success hash should be derived from the already-encoded return
value and contract-event bytes produced on the Rust side, so C++ does not have
to decode those bytes into XDR objects and then immediately walk the same data
again through `xdrSha256`.

## Mechanism

The Rust bridge already returns `encoded_invoke_result` and
`encoded_contract_events` as byte buffers. C++ then decodes every event in
`collectEvents`, decodes the result `SCVal` in `finalizeSuccess`, and calls
`xdrSha256(success)`, which serializes the `InvokeHostFunctionSuccessPreImage`
again to compute the hash. Returning either the final `success_hash` or an
encoded success-preimage blob from Rust would remove one full decode/re-encode
cycle per successful Soroban tx, while still allowing C++ to decode lazily only
when tx metadata actually needs structured XDR objects.

## Trigger

Run `scripts/run_apply_load_matrix.py` and profile
`InvokeHostFunctionApplyHelper::collectEvents`,
`InvokeHostFunctionApplyHelper::finalizeSuccess`, and `xdrSha256(success)`.
This should show the clearest win on `custom_token` and `soroswap`, where
successful transactions emit contract events on every invocation.

## Target Code

- `src/rust/src/soroban_proto_any.rs:488-516` — bridge already has encoded result/event bytes before returning to C++
- `src/transactions/InvokeHostFunctionOpFrame.cpp:707-736` — C++ decodes every returned contract event
- `src/transactions/InvokeHostFunctionOpFrame.cpp:816-827` — C++ decodes `result_value` and then hashes the typed preimage
- `src/protocol-curr/xdr/Stellar-ledger.x:526-530` — success preimage shape is only `SCVal returnValue` plus `ContractEvent events<>`
- `src/crypto/SHA.h:47-60` — `xdrSha256` walks the XDR object again to produce the hash

## Evidence

- The Rust side already materializes the exact encoded payloads needed for the
  success preimage and hands them to C++ as `RustBuf`s.
- C++ immediately turns those bytes back into `SCVal` / `ContractEvent`
  objects, and `xdrSha256(success)` serializes them again even though the hash
  only depends on the same logical payload.
- This work is entirely outside the Soroban VM, so any savings translate
  directly to apply-path overhead instead of host-engine internals.

## Anti-Evidence

- If tx-meta capture remains enabled, C++ still needs decoded `SCVal` and
  `ContractEvent` objects for metadata/event plumbing, so the isolated gain is
  limited to the hash path unless decode is also deferred.
- The bridge change must preserve byte-for-byte compatibility with the XDR hash
  of `InvokeHostFunctionSuccessPreImage`; any mismatch would be consensus-risky.
- SAC built-ins may emit fewer/smaller events than the other workloads, so the
  benefit may skew toward `custom_token` and `soroswap`.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete path from Rust bridge return (`soroban_proto_any.rs:488-516`) through C++ `collectEvents` (line 706-753) and `finalizeSuccess` (line 815-828). Confirmed that the Rust side returns pre-encoded XDR bytes (`encoded_invoke_result` as `RustBuf`, `encoded_contract_events` as `Vec<RustBuf>`), C++ decodes them via `xdr_from_opaque` into structured `SCVal`/`ContractEvent` objects, then `xdrSha256(success)` re-serializes the entire `InvokeHostFunctionSuccessPreImage` to compute the hash. The decode-then-re-encode cycle is real but the re-encode portion (the only avoidable work) is extremely lightweight.

### Code Paths Examined

- `src/rust/src/bridge.rs:34-55` — `InvokeHostFunctionOutput` struct: `result_value: RustBuf` and `contract_events: Vec<RustBuf>` are pre-encoded XDR bytes
- `src/rust/src/soroban_proto_any.rs:488-516` — Rust returns `encoded_invoke_result` and `encoded_contract_events` as raw XDR byte buffers
- `src/transactions/InvokeHostFunctionOpFrame.cpp:706-753` — `collectEvents`: decodes each event buffer via `xdr_from_opaque(buf.data, evt)` (line 735), but uses raw `buf.data.size()` for size metrics (line 717)
- `src/transactions/InvokeHostFunctionOpFrame.cpp:815-828` — `finalizeSuccess`: decodes `result_value` via `xdr_from_opaque` (line 819), then computes `xdrSha256(success)` (line 821) which re-serializes everything
- `src/transactions/InvokeHostFunctionOpFrame.cpp:772-813` — `setEvents`: requires decoded `ContractEvent` objects for event manager; called AFTER hash is computed
- `src/transactions/InvokeHostFunctionOpFrame.cpp:827` — `mOpMeta.setSorobanReturnValue(success.returnValue)` requires decoded `SCVal`
- `src/crypto/SHA.h:47-61` — `xdrSha256` uses `xdr::archive` to walk and serialize the XDR structure directly to SHA256 state (no temp buffer allocation)

### Findings

**The inefficiency is real but very small.** The `xdrSha256(success)` call re-serializes data that was available as raw XDR bytes. Since `InvokeHostFunctionSuccessPreImage` is `{SCVal returnValue; ContractEvent events<>;}`, its XDR encoding is exactly the concatenation of the raw result bytes, the 4-byte event count, and each raw event buffer — all of which are already available in `out.result_value.data` and `out.contract_events[i].data`.

**The decode step cannot be eliminated.** C++ needs decoded `ContractEvent` objects for `setEvents` → event manager (metadata), and the decoded `SCVal` for `mOpMeta.setSorobanReturnValue`. These uses are mandatory regardless of the hash computation path.

**The only avoidable work is the re-serialization inside `xdrSha256`.** This walks ~1KB of XDR data per tx (return value ~100 bytes + ~3 events × ~300 bytes each). The cost is on the order of 1–3 microseconds per transaction. For a 3200-tx benchmark, total savings would be ~3–10ms out of multi-second runs — well below the 5% threshold for Low severity.

**An alternative C++-only fix exists** that avoids modifying the Rust bridge: compute SHA256 directly from the raw buffers (`SHA256.add(result_value_bytes) + add(event_count_as_u32be) + add(each_event_bytes)`) instead of `xdrSha256(success)`. This is simpler than the hypothesis's proposed Rust-side change and equally correct since XDR encoding is canonical.

**Severity downgrade rationale:** The hypothesis claims Low severity (5–10% improvement). The actual improvement is <0.1% because the re-serialization cost is negligible relative to total per-tx cost (host execution, ledger access, entry processing all dominate). Downgraded to Informational.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp`, function `InvokeHostFunctionApplyHelper::finalizeSuccess` (lines 815-828)
- **Change description**: Replace `xdrSha256(success)` with direct SHA256 computation from raw buffers. Before decoding `result_value`, compute `SHA256(out.result_value.data || htonl(out.contract_events.size()) || out.contract_events[0].data || ...)`. Then proceed with decoding for metadata as before.
- **Correctness check**: `InvokeHostFunctionTests` in `src/transactions/test/InvokeHostFunctionTests.cpp` covers the success hash path. Any test that verifies `INVOKE_HOST_FUNCTION_SUCCESS` result will validate hash correctness.
- **Benchmark focus**: Per-tx post-host overhead. Expected improvement: <0.1% on any benchmark scenario. Not expected to be measurable in apply-load benchmarks.
