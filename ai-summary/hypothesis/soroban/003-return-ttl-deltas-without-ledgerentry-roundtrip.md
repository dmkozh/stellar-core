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
