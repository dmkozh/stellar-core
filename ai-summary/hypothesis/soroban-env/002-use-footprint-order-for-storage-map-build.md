# H002: Reuse Footprint Order Instead of Re-Deriving Keys from Decoded Entries

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: CPU / input-key reconstruction
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once `SorobanResources.footprint` has been decoded, the Rust boundary should use
that footprint order as the source of truth for associating input entry buffers
with ledger keys. It should not decode a `LedgerEntry`, reconstruct a fresh
`LedgerKey` from it, and then probe `FootprintMap` to confirm that the key was
already provided separately.

## Mechanism

The C++ side serializes footprint entries in a deterministic order:
`readOnly` first and `readWrite` second. Rust then decodes the footprint from
`encoded_resources`, but `build_storage_map_from_xdr_ledger_entries` ignores
that existing ordering and instead pays `ledger_entry_to_ledger_key` plus
`footprint.contains_key(...)` for every entry. For contract-data entries, key
reconstruction deep-clones the contract ID and `ScVal` key; for all entries it
allocates a new `Rc<LedgerKey>` only to rediscover membership in the already
decoded footprint.

A zip-based build path that iterates the original footprint vectors alongside
`encoded_ledger_entries` / `encoded_ttl_entries` could reuse the expected key
directly, removing the second key-construction pipeline and its map lookup from
every invocation.

## Trigger

Run any apply-load scenario. The effect is strongest on `custom_token` and
`soroswap`, where the footprint contains contract-data keys with non-trivial
`ScVal` structure instead of only simple account keys.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:507-523` — C++ sends read-only entries before read-write entries
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:426-444` — Rust already decodes `SorobanResources` before building the storage map
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:903-931` — `ledger_entry_to_ledger_key` reconstructs keys from decoded entries
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:959-1043` — storage-map builder derives keys and rechecks footprint membership for every entry

## Evidence

The order contract is already present in the implementation: C++ appends entry
buffers in footprint order, and Rust decodes the footprint before consuming the
buffers. That means the bridge currently pays for both representations of the
same association: ordered footprint keys and reconstructed keys from the entry
payload. The reconstructed-key path is especially wasteful for contract-data
keys, where `ledger_entry_to_ledger_key` clones structured `ScVal` key material
that Rust already had in the footprint it decoded a few lines earlier.

## Anti-Evidence

This optimization is correctness-sensitive because it turns the current
"derive-and-verify" behavior into an ordering contract between C++ and Rust; a
debug assertion or opt-in validation path may still be needed. It also does not
remove the actual `LedgerEntry` decode, so the savings ceiling is lower than a
full decoded-entry cache.
