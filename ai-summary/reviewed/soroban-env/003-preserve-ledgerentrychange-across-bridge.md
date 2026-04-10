# H003: Preserve `LedgerEntryChange` Across the Bridge Instead of Re-Synthesizing Effects

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: CPU / output marshaling
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The bridge should pass the host-produced per-footprint change descriptors to C++
directly. C++ should not need to infer deletions by omission, rebuild change
sets from decoded entries, or round-trip TTL changes through synthetic
`LedgerEntry` XDR just to apply the result of a Soroban invocation.

## Mechanism

`soroban-env-host` already returns `ledger_changes: Vec<LedgerEntryChange>` for
every footprint item, including `read_only`, `encoded_key`, optional
`encoded_new_value`, and optional `ttl_change`. But the bridge discards that
shape in `extract_ledger_effects()`: it emits only `modified_ledger_entries`,
serializes synthetic TTL `LedgerEntry`s, and throws away the encoded keys.

On the C++ side, `recordStorageChanges()` then decodes returned entries,
re-derives keys, inserts `UnorderedSet<LedgerKey>` trackers, and finally scans
the read-write footprint again to infer deletions from missing outputs. A bridge
struct that preserved the original `LedgerEntryChange` fields would let C++
apply writes / deletes / TTL bumps in one pass without synthetic TTL XDR and
without the omission-based bookkeeping.

## Trigger

Run `custom_token`, `soroswap`, or batched SAC apply-load scenarios. These
workloads drive repeated state updates, so the bridge pays the full
`extract_ledger_effects()` → `recordStorageChanges()` translation cost on every
successful invocation.

## Target Code

- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs:41-59` — host already returns `ledger_changes` for embedder consumption
- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs:95-119` — `LedgerEntryChange` already contains `encoded_key`, `encoded_new_value`, and `ttl_change`
- `src/rust/src/bridge.rs:34-54` — bridge collapses output to `modified_ledger_entries: Vec<RustBuf>`
- `src/rust/src/soroban_proto_any.rs:261-301` — `extract_ledger_effects()` discards key metadata and serializes synthetic TTL `LedgerEntry`s
- `src/transactions/InvokeHostFunctionOpFrame.cpp:610-703` — `recordStorageChanges()` rebuilds key sets and infers deletions by omission

## Evidence

The host has already done the hard work of producing a change descriptor per
footprint slot. The bridge then narrows that richer structure into a lossy list
of encoded ledger entries, forcing C++ to reconstruct information it previously
had for free. The most obvious waste is the TTL path: Rust serializes a fresh
`LedgerEntry(TTL)` XDR from `ttl_change`, then C++ immediately decodes it back
to a typed object before updating the ledger state.

## Anti-Evidence

C++ still needs to decode `encoded_new_value` for entries that are actually
upserted, so this does not eliminate all output-side decoding. The representation
change is also invasive: both the bridge schema and the C++ apply helper would
need to move from the current "list of encoded entries" model to a keyed change
model.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full output path from soroban-env-host's `InvokeHostFunctionResult.ledger_changes` through the Rust bridge's `extract_ledger_effects()` to C++'s `recordStorageChanges()`. Confirmed three sources of redundant work: (1) synthetic TTL `LedgerEntry` XDR construction and round-trip serialization (~40 bytes per TTL change), (2) key re-derivation via `LedgerEntryKey(le)` on the C++ side when `encoded_key` was already available on the Rust side, and (3) omission-based deletion inference requiring a full scan of the read-write footprint against an `UnorderedSet`. However, the per-invocation cost of all three is extremely small relative to total transaction processing time.

### Code Paths Examined

- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs:95-134` — `LedgerEntryChange` struct contains `read_only`, `encoded_key`, `encoded_new_value`, `ttl_change` with `key_hash`, `old_live_until_ledger`, `new_live_until_ledger`
- `src/rust/src/soroban_proto_any.rs:261-301` — `extract_ledger_effects()` iterates changes, discards `encoded_key` and `read_only` flag, pushes only `encoded_new_value` bytes, constructs synthetic `LedgerEntry(Ttl)` from `ttl_change` via `non_metered_xdr_to_rust_buf`
- `src/rust/src/soroban_proto_any.rs:488-516` — success path calls `extract_ledger_effects(res.ledger_changes)` and returns flat `modified_ledger_entries: Vec<RustBuf>`
- `src/rust/src/bridge.rs:34-54` — `InvokeHostFunctionOutput` struct has only `modified_ledger_entries: Vec<RustBuf>`, losing per-entry metadata
- `src/transactions/InvokeHostFunctionOpFrame.cpp:616-619` — C++ deserializes each `RustBuf` via `xdr_from_opaque`, derives key via `LedgerEntryKey(le)`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:630-658` — builds `createdAndModifiedKeys` and `createdKeys` UnorderedSets, validates, upserts
- `src/transactions/InvokeHostFunctionOpFrame.cpp:689-702` — scans entire read-write footprint to infer deletions by omission from `createdAndModifiedKeys`

### Findings

The inefficiency is mechanistically real:

1. **TTL round-trip serialization**: For each footprint entry with a TTL bump, Rust constructs a `LedgerEntry { data: LedgerEntryData::Ttl(TtlEntry { key_hash, live_until_ledger_seq }) }` struct, serializes it to ~44 bytes XDR via `non_metered_xdr_to_rust_buf`, and C++ immediately deserializes it back. A typical SAC transfer has ~5 TTL-bearing entries, so this is ~5 encode/decode round-trips per invocation.

2. **Key re-derivation**: C++ calls `LedgerEntryKey(le)` for every returned entry. The original `encoded_key` was available in `LedgerEntryChange` but discarded by `extract_ledger_effects()`. However, C++ would still need to decode `encoded_key` from XDR to get a typed `LedgerKey`, so the saving is only the key-derivation pattern match, not a full deserialization.

3. **Omission-based deletion**: The footprint scan at lines 689-702 is O(|readWrite footprint|) per invocation. With ~5-10 read-write entries, this is a trivial loop.

**Impact estimate**: Each TTL round-trip costs ~400ns (serialize + FFI + deserialize). Key derivation is ~200ns per entry. Deletion scan is ~500ns per invocation. For a transaction with 10 entries and 5 TTL changes, total overhead is ~5μs. At 6400 TX/ledger-close, that's ~32ms — roughly 0.2-0.6% of a multi-second ledger close. This is well below the 5% threshold for Low severity.

The fix is correct but highly invasive: it requires a new CXX bridge struct (replacing flat `Vec<RustBuf>` with a keyed change model), changes to `extract_ledger_effects()`, and rewriting `recordStorageChanges()` to consume the richer format. All changes remain within the bridge layer (no soroban-env-host internals), so it's in scope.

### PoC Guidance

- **Target code**: `src/rust/src/bridge.rs` (new bridge struct), `src/rust/src/soroban_proto_any.rs:261-301` (replace `extract_ledger_effects`), `src/transactions/InvokeHostFunctionOpFrame.cpp:610-703` (rewrite `recordStorageChanges`)
- **Change description**: Replace `modified_ledger_entries: Vec<RustBuf>` with a richer struct (e.g., `Vec<BridgeLedgerEntryChange>` with `is_deleted: bool`, `encoded_key: RustBuf`, `encoded_new_value: Option<RustBuf>`, `ttl_new_live_until: Option<u32>`, `ttl_key_hash: Option<RustBuf>`). Skip synthetic TTL XDR construction; pass TTL bumps as scalar fields. Mark deletions explicitly instead of inferring by omission.
- **Correctness check**: `[soroban]` and `[tx]` test tags cover this path extensively. Run full `[soroban]` suite to validate.
- **Benchmark focus**: apply-load SAC/custom_token at TX=6400,T=1. Expect <1% improvement — likely within noise. The value is architectural cleanliness, not performance.
