# H003: `getAllKeysWithoutSealing` Builds Ordered `std::set` (O(n log n)) for Membership-Only Use

**Date**: 2026-04-10
**Subsystem**: ledger (LedgerTxn, BucketManager)
**Severity**: Low
**Impact**: Reduce keyset construction from O(n log n) to O(n) amortized, saving ~5-15ms per ledger close
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`getAllKeysWithoutSealing` (LedgerTxn.cpp:1704-1722) constructs a
`LedgerKeySet` of all modified entries. Its sole consumer,
`resolveBackgroundEvictionScan` (BucketManager.cpp:1220-1222), only uses
`find()` for membership testing — never iteration in sorted order, never
range queries, never min/max. The function should return an O(1)-lookup
container (e.g., `UnorderedSet<LedgerKey>`) instead of the current
`std::set<LedgerKey, LedgerEntryIdCmp>`, eliminating the O(n log n) BST
insertion overhead with expensive field-by-field comparisons.

## Mechanism

`LedgerKeySet` is defined as `std::set<LedgerKey, LedgerEntryIdCmp>`
(types.h:18) — an ordered balanced BST. The `getAllKeysWithoutSealing`
function iterates all entries in the LedgerTxn's `mEntry` map and inserts
each key into this ordered set:

```cpp
LedgerKeySet result;    // std::set with LedgerEntryIdCmp ordering
for (auto const& [k, v] : mEntry)
{
    if (k.type() == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        result.emplace(k.ledgerKey());    // O(log n) BST insertion
    }
}
return result;
```

For n modified entries, BST insertion costs O(n log n) total comparisons.
Each comparison for CONTRACT_DATA keys invokes `LedgerEntryIdCmp` which
calls `lexCompare(contract, contract, key, key, durability, durability)`
(LedgerCmp.h:92-95). The `key` field is an `SCVal` — a deeply-nested XDR
union where comparison must recursively traverse the structure.

For the SAC benchmark with TX=3200: the LedgerTxn has ~15K-50K modified
entries. With log₂(50K)≈16 comparisons per insertion and ~100-200ns per
CONTRACT_DATA comparison:

- `std::set` construction: 50K × 16 × 150ns ≈ **120ms** (worst case)
- `std::unordered_set` construction: 50K × hash_cost ≈ 50K × 200ns ≈ **10ms**

Even at half the pessimistic estimate, this is a meaningful saving.
However, the actual entry count and comparison cost need profiling to
validate — early-exit on type mismatch and SCAddress comparison may
dominate over deep SCVal comparison.

**Note**: This hypothesis is complementary to H010 (reviewed: defer
construction). H010 eliminates the cost entirely when eviction candidates
are empty (always true in the benchmark). This hypothesis reduces the cost
when eviction candidates exist (production validators). The two
optimizations are independent and can be applied together.

## Trigger

Run `scripts/run_apply_load_matrix.py` for the SAC scenario (TX=3200).
Profile `LedgerTxn::Impl::getAllKeysWithoutSealing`. The `std::set`
insertion overhead should be visible as a fraction of
`finalizeLedgerTxnChanges` time.

For production impact: run on a validator node processing ledgers with
non-zero eviction candidates.

## Target Code

- `src/ledger/LedgerTxn.cpp:getAllKeysWithoutSealing:1704-1722` — builds `LedgerKeySet` (ordered set) from all modified entries
- `src/util/types.h:18` — `typedef std::set<LedgerKey, LedgerEntryIdCmp> LedgerKeySet`
- `src/bucket/LedgerCmp.h:LedgerEntryIdCmp:90-96` — CONTRACT_DATA comparison with `lexCompare` on SCAddress + SCVal
- `src/bucket/BucketManager.cpp:resolveBackgroundEvictionScan:1220-1222` — sole consumer, uses only `find()` for membership testing
- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:2972` — call site: `ltx.getAllKeysWithoutSealing()`
- `src/ledger/LedgerHashUtils.h` — `LedgerKeyHash` (SipHash-based) available for unordered containers

## Evidence

1. `resolveBackgroundEvictionScan` (BucketManager.cpp:1217-1242) uses `modifiedKeys.find()` at lines 1220 and 1222 — pure membership testing, never sorted iteration.
2. `LedgerKeyHash` already exists in `LedgerHashUtils.h` and is used by `UnorderedMap<LedgerKey, ...>` throughout the codebase — the hash infrastructure for an unordered set is already available.
3. The H010 reviewer explicitly noted the correction: "LedgerKeySet is `std::set<LedgerKey, LedgerEntryIdCmp>` — an **ordered** balanced tree, not an UnorderedSet" — confirming the ordered container choice was not intentional for performance.
4. `mEntry` (LedgerTxn's internal map) is already an `UnorderedMap<InternalLedgerKey, LedgerEntryPtr>` — the entries are NOT inherently ordered. Building an ordered set from unordered data is pure waste when the ordering is unused.
5. For the benchmark: even though H010 would eliminate this cost (lazy construction when no eviction candidates), the container type optimization provides a safety net and helps production performance independently.

## Anti-Evidence

1. H010 (reviewed/ledger/010) already targets this exact function and proposes lazy construction, which eliminates the cost entirely in benchmark scenarios. This hypothesis only provides incremental benefit beyond H010 for the production case.
2. `LedgerKeySet` is used in many other places (QueryServer.cpp, BucketListSnapshot.cpp, invariant checks, tests) — changing the typedef would affect all consumers. The fix should be scoped to `getAllKeysWithoutSealing`'s return type specifically, not the global typedef.
3. `LedgerKeyHash` uses SipHash which hashes all fields including complex `SCVal` keys — hash computation for CONTRACT_DATA keys may not be dramatically cheaper than the ordered comparison, especially with early-exit on type mismatch in `LedgerEntryIdCmp`.
4. In the benchmark, with H010 applied, this optimization provides zero additional benefit. The hypothesis is primarily valuable for production validators with actual eviction workloads.
5. The actual `mEntry` size in the benchmark depends on the workload and how entries are batched. If many transactions modify the same keys (e.g., same contract's storage), the effective n may be smaller than `TX_count × entries_per_tx`.
