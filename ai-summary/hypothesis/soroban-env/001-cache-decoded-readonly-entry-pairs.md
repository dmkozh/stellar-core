# H001: Cache Decoded Read-Only Entry/TTL Pairs in the Rust Bridge

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: CPU / Rust-side input decoding
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Repeated immutable read-only footprint entries should be decoded from XDR once
per apply-thread / protocol cache and then reused across invocations. The
bridge should not re-run `metered_from_xdr_with_budget::<LedgerEntry>` and
`::<TtlEntry>` for the same contract code / contract instance bytes on every
transaction in the same ledger close.

## Mechanism

`invoke_host_function` always rebuilds the storage map from encoded entry
buffers, and `build_storage_map_from_xdr_ledger_entries` blindly decodes every
ledger entry / TTL pair on every call. In the benchmark workloads, many of
those read-only entries are stable across hundreds or thousands of
transactions: custom-token transfers reuse the same token code + instance, and
soroswap swaps reuse the same router / pool code and contract-instance keys.

`ProtocolSpecificModuleCache` already exists as per-protocol mutable state that
is cloned once per apply thread and reused across many transactions. Extending
it with a read-only entry cache keyed by `(footprint key, serialized bytes)` (or
equivalent cache identity) would let the Rust bridge hand back pre-decoded
`Rc<LedgerEntry>` / `Rc<TtlEntry>` pairs for immutable inputs, avoiding repeated
XDR parse + allocation work on the hot path.

## Trigger

Run the standard apply-load `custom_token` or `soroswap` scenarios. Every
transaction in those workloads repeatedly imports the same read-only contract
code / contract-instance entries into Rust before host execution starts.

## Target Code

- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:408-447` — enforcing path always rebuilds storage from encoded buffers
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:959-1045` — per-entry `metered_from_xdr_with_budget` of `LedgerEntry` / `TtlEntry`
- `src/rust/src/soroban_proto_any.rs:711-831` — `ProtocolSpecificModuleCache` already carries reusable per-protocol cache state
- `src/rust/src/soroban_module_cache.rs:22-60` — module-cache clones are reused across many invocations
- `src/simulation/TxGenerator.cpp:840-865` — custom-token transfers reuse `instance.readOnlyKeys`
- `src/simulation/ApplyLoad.cpp:3075-3205` — soroswap benchmark generates many swaps against shared contracts
- `src/simulation/TxGenerator.cpp:3140-3149` — soroswap swaps reuse router / SAC / pair read-only keys
- `src/rust/src/soroban_test_wasm.rs:116-138` — apply-load token / soroswap Wasm blobs are embedded immutable assets

## Evidence

The current Rust boundary performs the expensive part of the read-only path
even when C++ side caching is perfect: it still parses the same encoded
`LedgerEntry` and `TtlEntry` payloads into fresh Rust objects on every
invocation. The benchmark generators make that repetition explicit by reusing
the same read-only keys across every custom-token and soroswap transaction, and
the existing `ProtocolSpecificModuleCache` is an obvious place to attach a
decoded-entry cache without introducing a brand-new lifetime mechanism.

## Anti-Evidence

The cache needs careful invalidation across protocol changes and any ledger
state transition that changes the serialized bytes for a supposedly cached key.
Savings are concentrated in T=1 and in the read-only-heavy scenarios; read-write
entries remain transaction-unique and still have to be decoded every time.
