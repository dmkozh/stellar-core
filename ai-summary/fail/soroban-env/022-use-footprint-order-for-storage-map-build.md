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

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the full invocation path from C++ (`InvokeHostFunctionApplyHelper::addReads` → `invokeHostFunction`) through the bridge layer (`soroban_proto_any.rs:invoke_host_function` → `invoke_host_function_or_maybe_panic` → `soroban_proto_all.rs:invoke_host_function_with_trace_hook_and_module_cache`) into `soroban-env-host`'s `e2e_invoke::invoke_host_function`. The key reconstruction (`ledger_entry_to_ledger_key`) and footprint membership check (`footprint.0.contains_key`) both occur inside `build_storage_map_from_xdr_ledger_entries` at `e2e_invoke.rs:959-1043`, which is entirely within the `soroban-env-host` crate. The bridge layer only passes `CxxBuf` byte iterators through — it has no mechanism to intercept or substitute pre-matched keys.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:360-502` — `addReads()` iterates footprint keys but skips entries that are expired, archived, or non-existent; the resulting `mLedgerEntryCxxBufs` is a SUBSET of footprint keys, not 1:1
- `src/rust/src/soroban_proto_any.rs:310-354` — bridge `invoke_host_function` receives `&Vec<CxxBuf>` entries, passes as iterators
- `src/rust/src/soroban_proto_any.rs:391-462` — `invoke_host_function_or_maybe_panic` passes entries through to the protocol adapter
- `src/rust/src/soroban_proto_all.rs:101-136` — p26 adapter forwards encoded iterators directly to `e2e_invoke::invoke_host_function`
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:408-447` — `invoke_host_function` decodes resources, builds footprint, then calls `build_storage_map_from_xdr_ledger_entries` with encoded iterators
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:959-1043` — `build_storage_map_from_xdr_ledger_entries` decodes every entry, reconstructs keys via `ledger_entry_to_ledger_key`, and checks footprint membership
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:1046-1050` — footprint keys with no matching entry are inserted as `None` (proving entries ≠ footprint keys)

### Why It Failed

Three independent reasons:

1. **Out of scope**: The key reconstruction (`ledger_entry_to_ledger_key`, line 983) and footprint membership check (`footprint.0.contains_key`, line 1036) both occur inside `soroban-env-host`'s `build_storage_map_from_xdr_ledger_entries`. The `e2e_invoke::invoke_host_function` API only accepts `T: AsRef<[u8]>` encoded byte iterators — there is no API to pass pre-matched key-entry pairs. Implementing this hypothesis requires modifying `e2e_invoke` inside `soroban-env-host`, which is explicitly out of scope per the soroban-env scope constraint.

2. **Entries are not 1:1 with footprint keys**: The C++ `addReads()` skips footprint keys whose entries are expired, archived, temporarily missing, or non-Soroban (lines 393-445, 448-472). The `mLedgerEntryCxxBufs` vector is a proper subset of the footprint. A simple zip of footprint keys with encoded entries would mis-associate keys with entries. The existing code at lines 1046-1050 handles this mismatch by iterating footprint keys and inserting `None` for unmatched ones — this logic depends on the key-from-entry reconstruction to determine which footprint entries have been populated.

3. **Budget metering correctness**: `ledger_entry_to_ledger_key` charges the per-TX `Budget` via `metered_clone` for each field. Skipping these charges would change the observable budget consumption of transactions, which is a protocol behavior change, not just an optimization.

### Lesson Learned

When the target function (`build_storage_map_from_xdr_ledger_entries`) is inside `soroban-env-host` and the bridge-layer API only accepts opaque byte iterators, the bridge layer cannot control how storage maps are built. Additionally, the hypothesis's "zip-based" assumption requires 1:1 correspondence between footprint keys and encoded entries, but the C++ side legitimately omits entries for expired/archived/missing keys, making a position-based approach incorrect without significant protocol-level changes.
