# H003: Return TTL Deltas Without Rebuilding Full LedgerEntry XDR

**Date**: 2026-04-09
**Subsystem**: soroban
**Severity**: Medium
**Impact**: CPU / allocation churn on Rust->C++ result marshaling
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When the Rust host reports that a key's TTL changed, the bridge should return a
compact TTL delta representation that C++ can apply directly. It should not need
to synthesize a full `LedgerEntry(TTL)` XDR blob in Rust and immediately decode
that blob back to a `LedgerEntry` in C++.

## Mechanism

`extract_ledger_effects` materializes each TTL bump by constructing a fresh
`LedgerEntryData::Ttl` object and serializing it into a `RustBuf`, and
`recordStorageChanges` then decodes every returned buffer back into a
`LedgerEntry`. Soroban writes and autorestores routinely touch TTLs alongside
contract data/code, so this bridge round-trip adds extra per-key allocation and
XDR work that does not carry any information beyond `(key_hash, new_live_until)`.

## Trigger

Run apply-load on `custom_token` or `soroswap` and profile successful write
transactions with many modified keys or autorestores. If the hypothesis is
correct, result processing will show repeated Rust-side `non_metered_xdr_to_rust_buf`
calls for TTL entries and matching C++ `xdr::xdr_from_opaque` work in
`recordStorageChanges`.

## Target Code

- `src/rust/src/soroban_proto_any.rs:extract_ledger_effects:261-301` — synthesizes full TTL `LedgerEntry` XDR for every TTL change
- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:497-515` — returns those encoded TTL entries in `modified_ledger_entries`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::recordStorageChanges:610-655` — decodes every returned entry, including TTL-only updates

## Evidence

The Rust bridge already has `ttl_change` as a dedicated lightweight structure,
but it expands that back into a full `LedgerEntry` only because the C++ bridge
result format has a single `modified_ledger_entries` byte-vector channel. The
C++ side does not use any TTL-specific information beyond what was already in the
delta, so the full XDR round-trip looks structurally unnecessary.

## Anti-Evidence

Combining TTL deltas with normal entry updates into one result stream simplifies
the existing C++ apply code, so splitting them requires an interface change
through the cxx bridge and careful handling of ordering. If TTL updates are only
a small fraction of total bridge traffic in a scenario, the gain may be smaller
than the input-side caching opportunities above.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

I traced the complete flow from the soroban host's `LedgerEntryChange` output through `extract_ledger_effects` (Rust) to `recordStorageChanges` (C++). The hypothesis correctly identifies that TTL entries are the **only** entries in `modified_ledger_entries` that undergo Rust-side XDR construction and serialization — ContractCode/ContractData entries arrive pre-encoded from the host via `encoded_new_value` and are passed through zero-copy. However, a TtlEntry serializes to only ~48 bytes of XDR (4-byte `last_modified_ledger_seq` + 4-byte discriminant + 32-byte `key_hash` + 4-byte `live_until_ledger_seq` + 4-byte ext), and typical transactions produce 1–6 TTL updates, making the total per-transaction overhead 48–288 bytes of XDR ser/deser — well under 1 microsecond.

### Code Paths Examined

- `src/rust/src/soroban_proto_any.rs:extract_ledger_effects:261-301` — Confirmed: for each `ttl_change` where `new > old`, constructs a `LedgerEntry{Ttl{...}}` and calls `non_metered_xdr_to_rust_buf`. This is ~48 bytes per TTL entry. Non-TTL entries at line 269-270 are zero-cost pass-through of pre-encoded `encoded_new_value`.
- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:497` — Confirmed: calls `extract_ledger_effects` and puts result in `modified_ledger_entries`.
- `src/rust/src/bridge.rs:34-55` — `InvokeHostFunctionOutput` has a single `modified_ledger_entries: Vec<RustBuf>` channel. Adding a separate TTL channel would require modifying this shared CXX bridge struct.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:610-659` — Confirmed: `xdr_from_opaque` deserializes each buffer. For TTL entries (~48 bytes), this is trivial. The code already branches on `lk.type() != TTL` for write fee accounting. Even with the optimization, the C++ side would still need to construct a `LedgerEntry` and `LedgerKey` for `upsertLedgerEntry`, and still track `createdAndModifiedKeys` and `createdKeys`.

### Why It Failed

The inefficiency is real but the per-invocation cost is negligible. A TtlEntry XDR is ~48 bytes; serializing/deserializing 48 bytes takes on the order of 100 nanoseconds. With 1–6 TTL entries per Soroban transaction, the total overhead is ~0.1–0.6 microseconds per transaction. Soroban transactions take 1–10 milliseconds to execute (host + VM), meaning the TTL XDR round-trip represents roughly 0.01–0.06% of total transaction cost. This cannot produce a measurable benchmark improvement at any transaction volume.

Furthermore, the optimization requires a non-trivial CXX bridge interface change (new shared struct, new `Vec` field on `InvokeHostFunctionOutput`), and the C++ side would still need to construct `LedgerEntry`/`LedgerKey` objects for the upsert path, reducing the actual savings further.

### Lesson Learned

When evaluating bridge marshaling optimizations, check the **XDR size** of the entries being marshaled and how many occur per transaction. Pre-encoded pass-through entries (like `encoded_new_value`) have zero marshaling cost on the Rust side. Only entries constructed and serialized in the bridge layer (like TTL entries) incur actual ser/deser cost, and their small size (~48 bytes) makes them negligible compared to host execution time.
