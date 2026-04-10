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
