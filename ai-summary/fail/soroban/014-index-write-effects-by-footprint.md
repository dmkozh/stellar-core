# H002: Index Write Effects by RW Footprint Instead of Rehashing Keys in C++

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Informational
**Impact**: C++ post-host bookkeeping / wide-footprint Soroban transactions
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The Rust host should return write effects in a form that preserves their
identity relative to the transaction's declared read-write footprint, so C++ can
apply updates and deletions directly. C++ should not need to reconstruct
membership by hashing every modified key into temporary sets and then rescanning
the entire footprint to discover which keys were deleted.

## Mechanism

`extract_ledger_effects` drops footprint position and returns only a flat
`Vec<RustBuf>` of modified entries, with deletions represented implicitly by
omission. `recordStorageChanges` therefore decodes every returned entry, hashes
its `LedgerKey` into `createdAndModifiedKeys` / `createdKeys`, and then loops
over the full RW footprint again to infer erases. On wide-footprint benchmark
transactions this creates avoidable hash-table churn on complex
`CONTRACT_DATA` keys whose ordering is already known from the input footprint; a
result shape like `(rw_index, encoded_entry)` plus `deleted_rw_indices` would
turn the whole path into direct index-based application.

## Trigger

Run apply-load SAC with `APPLY_LOAD_BATCH_SAC_COUNT = 100` or the soroswap
benchmark. Profile `recordStorageChanges`; if the hypothesis is right, the hot
path will show time in `LedgerEntryKey(le)`, `createdAndModifiedKeys.insert`,
`createdKeys.insert`, and the final RW-footprint membership scan, even when the
write footprint is fixed and already ordered.

## Target Code

- `src/rust/src/soroban_proto_any.rs:extract_ledger_effects:261-301` — returns only encoded updated entries, not footprint indices or explicit deletions
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::recordStorageChanges:610-703` — decodes every effect into hash sets and rescans the RW footprint to infer erases
- `src/simulation/TxGenerator.cpp:invokeBatchTransfer:1480-1515` — batched SAC creates one TX with ~101 RW keys (source balance + 100 destinations)
- `docs/apply-load-benchmark-sac.cfg:33-37` — benchmark amplifies this path with `APPLY_LOAD_BATCH_SAC_COUNT = 100`

## Evidence

For batched SAC, a single successful TX can return roughly 101 modified balance
entries plus 101 TTL entries, and `recordStorageChanges` hashes each decoded key
before doing another membership pass over the original 101-key RW footprint.
This extra work is not inherent to ledger application; it is a consequence of
the bridge format discarding the footprint index that already exists on the C++
side.

## Anti-Evidence

C++ still has to deserialize every updated ledger entry into typed `LedgerEntry`
objects before it can write state, so this optimization only removes key-hash
bookkeeping and delete inference, not the full output-processing cost. If most
benchmark transactions update nearly every RW key anyway, the deletion side of
the win is smaller and the improvement may remain below the benchmark noise
floor.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the complete path from the soroban host's `LedgerEntryChange` output
through `extract_ledger_effects` (Rust bridge) to `recordStorageChanges` (C++).
The hypothesis correctly identifies real hash-table operations (key extraction,
set insertion, membership scan) in `recordStorageChanges`. However, the proposed
fix — returning footprint indices from the Rust bridge — requires building a
key→index mapping on the Rust side, which is essentially the same hash-based
matching work currently done on the C++ side. The work is moved, not eliminated.

### Code Paths Examined

- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:get_ledger_changes:183-293` — Host iterates `storage.map` (a sorted `MeteredOrdMap`) to produce `Vec<LedgerEntryChange>`. Each change carries `encoded_key: Vec<u8>`, `read_only: bool`, `encoded_new_value: Option<Vec<u8>>`, `ttl_change: Option<...>`. **No footprint index is provided by the host.**
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:LedgerEntryChange:99-119` — Struct definition confirms no index field exists. Changes are ordered by `storage.map` key sort, not footprint position.
- `src/rust/src/soroban_proto_any.rs:extract_ledger_effects:261-301` — Filters to RW entries with `encoded_new_value` plus TTL bumps. Returns flat `Vec<RustBuf>`. Bridge does NOT have access to the deserialized footprint (it passes `resources_buf` as opaque bytes to the host at line 447).
- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:443-457` — Bridge passes `resources_buf` to host; does not deserialize footprint itself.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:recordStorageChanges:610-703` — For each modified entry: `xdr_from_opaque` (~2µs for balance entries), `LedgerEntryKey(le)` (~0.3µs), hash set insert (~0.3µs), `upsertLedgerEntry`. Then scans 101 RW footprint keys for deletions (~30µs total).
- `src/util/types.cpp:LedgerEntryKey:21-72` — Constructs `LedgerKey` by copying fields from `LedgerEntry`. For CONTRACT_DATA: copies `SCAddress` + `SCVal` key + durability.
- `src/ledger/LedgerHashUtils.h:136-200` — `hash<LedgerKey>` for CONTRACT_DATA calls `xdrComputeHash(lk.contractData().key)` to hash the SCVal key tree. For SAC balance keys (Symbol + Address), this is ~0.3µs.
- `src/rust/src/bridge.rs:InvokeHostFunctionOutput:34-55` — CXX bridge struct has only `modified_ledger_entries: Vec<RustBuf>`. Adding index-based fields requires modifying this shared struct.

### Why It Failed

The optimization is fundamentally a **relocation of work from C++ to Rust**, not an elimination of work:

1. **Footprint indices aren't free**: The soroban host's `LedgerEntryChange` does NOT carry footprint indices (confirmed in `e2e_invoke.rs:99-119`). Changes come in `storage.map` key-sorted order, not footprint order. To compute indices, the Rust bridge would need to: (a) deserialize the footprint from `resources_buf` (~50-100µs for 101 CONTRACT_DATA keys), (b) build a `HashMap<Vec<u8>, u32>` from encoded keys to indices (~50µs), and (c) look up each change's `encoded_key` (~50µs for 202 lookups). This Rust-side work (~100-200µs) is comparable to the C++-side work being eliminated (~150µs).

2. **`xdr_from_opaque` dominates and is retained**: C++ must deserialize each returned entry to a typed `LedgerEntry` for `upsertLedgerEntry` (line 655). For 202 entries at ~2µs each, this is ~400µs — retained regardless of optimization. The eliminable key extraction + hash set operations (~150µs) are a fraction of the retained deserialization cost.

3. **Cost estimates for batched SAC (worst case)**:
   - C++-side savings: 202 × `LedgerEntryKey` (~60µs) + 202 × hash insert (~60µs) + 101 × footprint scan (~30µs) = **~150µs**
   - Rust-side additions: footprint deser (~75µs) + hash map build (~50µs) + 202 lookups (~50µs) = **~175µs**
   - **Net savings: ≈0** (possibly negative)
   - Host execution: ~3000-10000µs
   - Even if Rust-side work were free, improvement: ~150µs / ~5000µs = **~3%** (Informational)

4. **Normal Soroban transactions (5-10 entries)**: The eliminable overhead is ~10µs total, <1% of transaction time, well below noise floor.

5. **Substantial interface complexity required**: Modifying the CXX bridge `InvokeHostFunctionOutput` struct, `extract_ledger_effects`, and `recordStorageChanges` for negligible net benefit.

### Lesson Learned

When a hypothesis proposes moving post-processing work across the FFI boundary (e.g., from C++ to Rust), verify that the proposed Rust-side computation (building index maps, deserializing footprints) doesn't simply replace the C++-side work with equivalent work. The host's `LedgerEntryChange` ordering (sorted by key, not by footprint position) means footprint index computation is inherently a key-matching problem regardless of which side performs it.
