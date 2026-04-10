# H004: Host writeback pays a full hash-set diff to prove that no benchmark keys were deleted

**Date**: 2026-04-10
**Subsystem**: transactions
**Severity**: Low
**Impact**: writeback hash churn / redundant full-footprint scan
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The bridge output should describe deletions directly, so the C++ writeback path only processes keys that were actually removed. It should not have to hash every returned entry and then rescan the full read-write footprint to infer an empty deletion set for transfer/swap workloads that only create or update entries.

## Mechanism

`InvokeHostFunctionOutput` only returns surviving `modified_ledger_entries`, so `recordStorageChanges` reconstructs a `createdAndModifiedKeys` set, tracks `createdKeys`, and then scans every RW footprint key to discover which ones were omitted and therefore deleted. The apply-load model transactions do not exercise entry-deletion-heavy contracts — they transfer balances, update trustlines, and mutate pool state — so this set-diff is usually proving "nothing deleted" after paying for two hash sets and a second full RW-footprint pass.

## Trigger

Run `scripts/run_apply_load_matrix.py` on `sac,TX=6400,T=8` or `soroswap,TX=1600,T=8` and profile `InvokeHostFunctionApplyHelper::recordStorageChanges`. Expect time in `createdAndModifiedKeys.insert`, `createdKeys.insert`, and the final `for (lk : readWrite)` scan even when the model contracts only create/update balances and pair state.

## Target Code

- `src/rust/src/bridge.rs:InvokeHostFunctionOutput:30-55` — bridge output has no explicit deleted-entry list
- `src/rust/src/soroban_proto_any.rs:304-310` — omitted ledger entries are interpreted as deletions
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::recordStorageChanges:614-702` — builds two hash sets and rescans the RW footprint to infer deletions
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSacPayments:2105-2148` — batch-transfer benchmark creates or updates balances, then prevalidates
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateTokenTransfers:2323-2342` — token benchmark is pure transfer/update workload
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSoroswapSwaps:3140-3168` — Soroswap benchmark updates trustlines, balances, and pair state, not delete-heavy paths

## Evidence

The Rust-side contract invocation comment states that "ledger entries not returned have been deleted," and the C++ side implements exactly that inference with `createdAndModifiedKeys` plus a second scan over `mResources.footprint.readWrite`. The benchmark generators target transfer/swap workloads where deletions are atypical, so the generic deletion-recovery protocol spends apply time on a negative proof nearly every transaction.

## Anti-Evidence

The current interface is simple and correct for arbitrary contracts, and some real contracts can delete entries, so the optimization likely requires a bridge-format extension such as deleted RW indices or an explicit deletion vector. If the extra output shape complicates host compatibility too much, the practical win may end up limited to benchmark-oriented builds.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced `recordStorageChanges` (lines 629-724 of InvokeHostFunctionOpFrame.cpp) end-to-end. Confirmed that `createdAndModifiedKeys` (UnorderedSet<LedgerKey>) is built exclusively for deletion detection and has no other consumer. The Rust-side `extract_ledger_effects` (soroban_proto_any.rs:261-301) discards deletion information that `LedgerEntryChange` already provides (when `read_only == false && encoded_new_value.is_none()`). The C++ side then reconstructs this information via N hash-set inserts + M hash-set lookups. For benchmark workloads with zero deletions, the entire deletion scan (lines 709-722) is always a no-op that proves the empty set.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:recordStorageChanges:629-724` — `createdAndModifiedKeys` is declared at line 634, populated at line 650 (one insert per modified entry), and consumed only at line 711 (one lookup per RW footprint key). `createdKeys` (line 635/677) is a separate set used for correctness assertions and is unaffected.
- `src/rust/src/soroban_proto_any.rs:extract_ledger_effects:261-301` — Iterates `LedgerEntryChange` vector; for non-readonly entries, only pushes entries with `Some(encoded_new_value)`. Entries with `None` (deletions) are silently dropped — the information exists but is not passed through.
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:LedgerEntryChange:99-119` — Struct has `encoded_new_value: Option<Vec<u8>>` where `None` signals deletion; `encoded_key: Vec<u8>` provides the encoded key. Deletion info is available on the Rust side.
- `src/ledger/LedgerHashUtils.h:hash<LedgerKey>:136-200` — For CONTRACT_DATA keys (dominant in Soroban), hashing calls `xdrComputeHash(lk.contractData().key)` which constructs `XDRShortHasher`, acquires the global `gKeyMutex`, and streams the SCVal through SipHash-2,4. For TTL keys, just `std::hash<uint256>` (~10ns).
- `src/rust/src/bridge.rs:InvokeHostFunctionOutput:34-54` — CXX bridge struct with `modified_ledger_entries: Vec<RustBuf>` and no deletion field.

### Findings

**The inefficiency is real.** `createdAndModifiedKeys` is built solely for deletion detection. Each insert copies a `LedgerKey` (heap allocation for SCVal contents) and computes its hash (xdrComputeHash for CONTRACT_DATA keys, ~100-300ns). The deletion scan re-hashes every RW footprint key. For the common benchmark case where all RW entries survive, the entire scan proves the empty set.

**The impact is small.** Per-entry cost is ~150-300ns (hash + copy + insert), yielding ~30-90µs per tx for typical footprint sizes (10-200 entries). The deletion scan adds another ~20-60µs. Total overhead per 6400-tx benchmark run: ~300-600ms, which is <2% of total apply time. The `xdrComputeHash` for simple balance keys (Address SCVal) is lightweight, and TTL key hashing avoids shortHash entirely.

**A simpler C++-only optimization exists.** Instead of the bridge format change, count non-TTL modified entries during the main loop. If the count equals `readWrite.size()`, skip the deletion scan entirely (all RW keys survived). This eliminates the M hash lookups in the scan for the zero-deletion case. The `createdAndModifiedKeys` set would still be built but could be deferred to a lazy path only materialized when `modifiedRWCount < readWrite.size()`.

**The bridge already has deletion info.** `LedgerEntryChange.encoded_new_value == None` signals deletion, and `LedgerEntryChange.encoded_key` provides the encoded key. `extract_ledger_effects` currently discards this. Passing it through would eliminate `createdAndModifiedKeys` entirely, but the CXX bridge struct change is non-trivial.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp:recordStorageChanges:629-724`
- **Change description (simple, C++-only)**: Add a `size_t modifiedRWCount = 0` counter. In the modified entries loop (line 636-679), increment for each entry where `lk.type() != TTL`. After the loop, wrap the deletion scan (lines 709-722) in `if (modifiedRWCount < mResources.footprint.readWrite.size())`. This eliminates the deletion scan cost in the zero-deletion common case. Optionally, also defer `createdAndModifiedKeys` construction to only the deletion-detected branch by keeping a `vector<LedgerKey>` and building the set lazily.
- **Change description (full, bridge format)**: Add `deleted_entry_keys: Vec<RustBuf>` to `InvokeHostFunctionOutput` in `bridge.rs`. In `extract_ledger_effects`, collect encoded keys where `!read_only && encoded_new_value.is_none()`. In C++, replace the hash-set-based deletion detection with direct iteration of the (usually empty) deleted keys vector.
- **Correctness check**: Existing Soroban test suite covers both creation and deletion paths. Tests tagged `[soroban]` exercise `InvokeHostFunctionOpFrame`. The `createdKeys` assertion loop (lines 684-702) must remain unchanged.
- **Benchmark focus**: Profile `recordStorageChanges` wall time per-tx, specifically hash-set insert/find operations. Expect <2% improvement in overall ledger close time on `sac,TX=3200,T=8` benchmark. The simple count-based optimization should show the deletion scan disappearing from profiles.
