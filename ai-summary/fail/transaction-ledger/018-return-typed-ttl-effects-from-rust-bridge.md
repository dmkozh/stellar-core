# H002: Return Typed TTL Effects Instead of XDR-Encoded Synthetic `TTLEntry` Objects

**Date**: 2026-04-10
**Subsystem**: transaction-ledger (transactions/InvokeHostFunctionOpFrame, soroban-env bridge)
**Severity**: High
**Impact**: Post-host bridge overhead for frequent TTL-only effects
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When the host only bumps TTL, the bridge should return a compact typed TTL
effect (for example key-hash plus new `liveUntilLedger`) rather than first
encoding a synthetic `LedgerEntry{TTL}` in Rust and then decoding it back to a
full `LedgerEntry` in C++.

## Mechanism

`extract_ledger_effects` manufactures a synthetic TTL `LedgerEntry`, XDR-encodes
it, and appends it to `modified_ledger_entries`; `recordStorageChanges` then
decodes that buffer back into a `LedgerEntry`, reconstructs the key, validates
it, inserts it into hash sets, and routes it through the generic upsert path.
Apply-load workloads repeatedly extend TTL for shared contract-code / instance
entries and for touched balance entries, so the bridge pays this full encode →
decode → hash/set pipeline on a large stream of TTL-only updates that already
have a simpler representation in Rust.

## Trigger

Run `scripts/run_apply_load_matrix.py` for `custom_token` and `soroswap`,
especially `T=8`, and count how many elements of
`out.modified_ledger_entries` are TTL entries versus non-TTL entries. Profile
`extract_ledger_effects`, `xdr::xdr_from_opaque` inside
`recordStorageChanges`, and the number of TTL-only updates flowing through the
generic modification path.

## Target Code

- `src/rust/src/soroban_proto_any.rs:261-301` — synthesizes and encodes TTL `LedgerEntry` objects for `ttl_change`
- `src/rust/src/bridge.rs:34-55` — current bridge output shape only exposes `modified_ledger_entries: Vec<RustBuf>`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:610-703` — decodes every returned entry, including TTL-only effects
- `src/transactions/ParallelApplyUtils.cpp:771-786` — C++ already has special handling for read-only TTL bumps once it identifies them
- `src/simulation/ApplyLoad.cpp:3140-3149` — soroswap carries 5 shared read-only keys that can all produce repeated TTL effects
- `src/simulation/TxGenerator.cpp:840-865` — custom-token transfers reuse shared read-only contract keys every tx

## Evidence

- Rust already has the TTL effect in structured form (`ttl_change`) before it builds the synthetic `LedgerEntry`.
- C++ distinguishes TTL-only read-only bumps from real writes later in the pipeline, which means the typed information is semantically useful on the C++ side too.
- The current bridge shape forces two XDR transforms and multiple hash/set operations for each TTL-only effect.

## Anti-Evidence

- This requires a bridge-shape change on both sides, so the implementation is broader than a local micro-optimization.
- Some TTL effects correspond to read-write entries and must still preserve creation/deletion semantics carefully.
- If the host returns relatively few TTL-only effects in a given benchmark, the benefit may fall below the claimed severity.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the complete pipeline from `extract_ledger_effects` (Rust) through the CXX bridge to `recordStorageChanges` (C++) and into `commitChangesFromSuccessfulTx` (parallel apply). The inefficiency described is real: synthetic TTL `LedgerEntry` objects are XDR-encoded in Rust (~48 bytes per entry), crossed the bridge, then decoded in C++ and processed through the generic entry pipeline. However, the per-entry cost is trivially small and the total entry count per ledger is modest, making the cumulative overhead unmeasurable against overall ledger close time.

### Code Paths Examined

- `src/rust/src/soroban_proto_any.rs:261-301` — Confirmed: builds a `LedgerEntry{TTL}` from `ttl_change.key_hash` + `ttl_change.new_live_until_ledger`, XDR-encodes to ~48 bytes via `non_metered_xdr_to_rust_buf`
- `src/rust/src/soroban_proto_any.rs:149-166` — `non_metered_xdr_to_rust_buf` allocates a `Vec<u8>` and writes XDR into it
- `src/rust/src/bridge.rs:34-55` — `InvokeHostFunctionOutput.modified_ledger_entries: Vec<RustBuf>` mixes TTL entries with real data entries
- `src/transactions/InvokeHostFunctionOpFrame.cpp:616-659` — `recordStorageChanges` loop: `xdr_from_opaque` decodes each entry, `LedgerEntryKey` extracts key, inserts into `createdAndModifiedKeys` hash set, calls `upsertLedgerEntry`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:637` — TTL entries explicitly skip write metering (`if (lk.type() != TTL)`)
- `src/transactions/TransactionUtils.cpp:1974-2002` — `validateContractLedgerEntry` is a no-op for TTL keys (only checks CONTRACT_CODE and CONTRACT_DATA sizes)
- `src/util/types.cpp:64-66` — `LedgerEntryKey` for TTL: single 32-byte copy
- `src/ledger/LedgerHashUtils.h:194-197` — LedgerKey hash for TTL: `hashMix` over `uint256` (fast non-crypto hash)
- `src/transactions/ParallelApplyUtils.cpp:150-163` — `buildRoTTLSet` constructs per-tx hash set of RO TTL keys, used later in `commitChangesFromSuccessfulTx`
- `src/transactions/ParallelApplyUtils.cpp:830-856` — `commitChangeFromSuccessfulTx` routes RO TTL bumps to deferred `mRoTTLBumps` map
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:99-133` — Host's `LedgerEntryChange` already carries `ttl_change: Option<LedgerEntryLiveUntilChange>` with `key_hash: Vec<u8>` and `new_live_until_ledger: u32`

### Why It Failed

**The inefficiency exists but is far too small to produce measurable improvement.** Detailed cost analysis:

1. **Per-entry XDR cost is trivial**: A TTL `LedgerEntry` serializes to ~48 bytes (4B lastModifiedLedgerSeq + 4B discriminator + 32B keyHash + 4B liveUntilLedgerSeq + 4B ext). XDR encode/decode of 48 bytes is essentially memory copies, taking ~50-100ns each.

2. **Per-entry processing cost is minimal**: `LedgerEntryKey` for TTL is a single 32-byte copy (~20ns). `validateContractLedgerEntry` is a no-op for TTL. Hash set insertion uses a fast non-crypto hash of uint256 (~50ns). The `upsertLedgerEntry` call would still be required regardless of bridge shape.

3. **Total entry counts are modest**: Custom_token transfers have 2 RO + 2 RW soroban entries → up to 4 TTL entries per TX. Soroswap swaps have 5 RO + 3 RW soroban entries → up to 8 TTL entries per TX. At 1600 TXs (custom_token T=8) that's ~6400 TTL entries; at 1000 TXs (soroswap T=8) that's ~8000 TTL entries.

4. **Cumulative overhead is negligible**: 8000 entries × ~400ns per entry (encode + allocate + decode + key extract + hash insert) ≈ 3.2ms. Ledger close times for these workloads are in the 2-20 second range, making this <0.1% of total time.

5. **The proposed fix wouldn't eliminate the dominant cost**: The `upsertLedgerEntry` call—which is the actual state mutation—would remain identical regardless of whether the TTL arrived as XDR or as a typed struct. The savings are exclusively in the ~400ns of serialization/deserialization overhead per entry.

6. **Implementation cost is high**: Changing the CXX bridge `InvokeHostFunctionOutput` struct requires coordinated changes to the Rust bridge definition, the Rust `extract_ledger_effects` function, and the C++ `recordStorageChanges` method, plus updating the assertion logic that checks `createdKeys` TTL pairing. This complexity is disproportionate to the ~3ms total savings.

### Lesson Learned

XDR encode/decode overhead for small fixed-size entries (TTL = ~48 bytes) is negligible. Bridge shape optimizations should focus on entries with large or variable-size payloads (ContractData, ContractCode) where the serialization cost is meaningful, not on tiny fixed-size entries. The mere existence of an encode→decode round-trip does not imply significant overhead when the payload is small and the entry count is bounded.
