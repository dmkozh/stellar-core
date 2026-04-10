# H010: Defer `getAllKeysWithoutSealing()` Set Construction in `finalizeLedgerTxnChanges`

**Date**: 2026-04-10
**Subsystem**: ledger (LedgerManagerImpl, LedgerTxn, BucketManager)
**Severity**: Low
**Impact**: Eliminate ~2-8ms of wasted hash-set construction per ledger close when eviction produces zero candidates
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`finalizeLedgerTxnChanges` should not build an O(n) `LedgerKeySet` of all
modified entries when the eviction scan produces zero candidates to filter.
The key set should be constructed lazily — only when
`resolveBackgroundEvictionScan` determines there are eviction candidates
that need filtering against the current ledger's modifications.

## Mechanism

At `LedgerManagerImpl.cpp:2967`, `finalizeLedgerTxnChanges` unconditionally
calls `ltx.getAllKeysWithoutSealing()` which iterates the outermost
LedgerTxn's `mEntry` hash map (~15,000-60,000 entries for SAC/custom_token
benchmarks) and copies every `LEDGER_ENTRY`-typed `InternalLedgerKey` into
a new `LedgerKeySet` (`UnorderedSet<LedgerKey>`). Each insertion involves:
1. Extracting the `LedgerKey` from the `InternalLedgerKey` (~40-100 byte copy)
2. Computing the `LedgerKey` hash via `SipHash` (includes hashing SCVal
   fields for CONTRACT_DATA keys — expensive for complex keys)
3. Hash table bucket insertion

The resulting set is passed to `BucketManager::resolveBackgroundEvictionScan`
(line 1183) where it is used solely to filter eviction candidates (lines
1217-1242): for each candidate, it checks `modifiedKeys.find(getTTLKey(...))`
and `modifiedKeys.find(LedgerEntryKey(...))`.

In the apply-load benchmark, `APPLY_LOAD_BL_SIMULATED_LEDGERS = 0` means
the BucketList is nearly empty. All entries are freshly created with maximum
TTLs. The eviction scan produces **zero** eligible candidates. The filtering
loop at line 1217 never executes, making the entire key set construction
wasted work.

Even in production, validators processing fresh ledgers with no expired
entries will routinely have zero eviction candidates. The key set is only
useful when entries actually need to be evicted.

For the SAC benchmark with `TX=6400` and `APPLY_LOAD_BATCH_SAC_COUNT=100`:
each tx modifies ~200 CONTRACT_DATA entries + ~200 TTL entries + 1 source
account ≈ ~400 entries. With ~21,000 unique genesis accounts, the total
modified entries in the LedgerTxn ≈ ~40,000-60,000. At ~100-200ns per
SipHash + insertion: **~4-12ms** per ledger close, entirely wasted.

## Trigger

Run `scripts/run_apply_load_matrix.py` with any scenario. Profile
`finalizeLedgerTxnChanges` and specifically `getAllKeysWithoutSealing`.
The key set construction time should be visible in Tracy or perf as a
significant fraction of `finalizeLedgerTxnChanges` wall time.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:2967` — unconditional `ltx.getAllKeysWithoutSealing()` call
- `src/ledger/LedgerTxn.cpp:LedgerTxn::Impl::getAllKeysWithoutSealing:1704-1722` — iterates `mEntry` map, copies keys into `LedgerKeySet`
- `src/bucket/BucketManager.cpp:resolveBackgroundEvictionScan:1181-1290` — sole consumer of the key set; uses it only in lines 1217-1242
- `src/bucket/BucketManager.cpp:resolveBackgroundEvictionScan:1204` — `mEvictionFuture.get()` resolves candidate count BEFORE the key set is needed

## Evidence

- `getAllKeysWithoutSealing` (LedgerTxn.cpp:1709-1721) creates a new `LedgerKeySet` and iterates ALL `mEntry` entries, calling `result.emplace(k.ledgerKey())` for each — no lazy evaluation, no short-circuit
- The key set is passed by const reference to `resolveBackgroundEvictionScan` — it cannot be deferred after the call
- `resolveBackgroundEvictionScan` resolves the eviction future at line 1204 (`mEvictionFuture.get()`) BEFORE using the key set at line 1217 — the candidate count is known before filtering begins
- Benchmark config has empty BucketList (`APPLY_LOAD_BL_SIMULATED_LEDGERS = 0`), guaranteeing zero eviction candidates
- The `LedgerKey` hash function (LedgerHashUtils.h) uses `SipHash` and must hash all fields including complex SCVal keys for CONTRACT_DATA — this is significantly more expensive than simple integer hashing
- `fail/transaction-ledger/010-cache-getttlkey-sha256.md` demonstrated that LedgerKey hashing is expensive enough that a caching approach using `UnorderedMap<LedgerKey, LedgerKey>` actually REGRESSED performance due to hash overhead — this confirms LedgerKey hashing cost is material

## Anti-Evidence

- The key set construction is O(n) in modified entries, which is already linear in work that must happen anyway (getAllEntries also iterates mEntry)
- In production with a mature BucketList, eviction candidates may be non-zero, requiring the key set — the optimization must not break this path
- Restructuring the API to accept a predicate or LedgerTxn reference instead of a materialized set requires changing the `resolveBackgroundEvictionScan` interface
- An alternative approach (resolve eviction first, then conditionally build the set) would require splitting `resolveBackgroundEvictionScan` into candidate-resolution and filtering phases
- The overhead may be partially masked by the subsequent `getAllEntries` call (line 3045) which also iterates `mEntry` and may warm the cache

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced from `finalizeLedgerTxnChanges` (LedgerManagerImpl.cpp:2942) through
`getAllKeysWithoutSealing` (LedgerTxn.cpp:1704-1722) into
`resolveBackgroundEvictionScan` (BucketManager.cpp:1181-1290). Confirmed that
the key set is constructed unconditionally before the eviction future resolves,
and the filter loop (lines 1217-1242) is the sole consumer. When
`eligibleEntries` is empty (guaranteed in benchmarks with empty BucketList),
the entire set construction is provably wasted. The optimization is correct
and preserves all safety invariants.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:2942-2963` — unconditionally calls `ltx.getAllKeysWithoutSealing()` and passes result to `resolveBackgroundEvictionScan`
- `src/ledger/LedgerTxn.cpp:Impl::getAllKeysWithoutSealing:1704-1722` — iterates entire `mEntry` map, copies each `LEDGER_ENTRY`-typed key into a new `LedgerKeySet` via `result.emplace(k.ledgerKey())`
- `src/bucket/BucketManager.cpp:resolveBackgroundEvictionScan:1181-1242` — resolves `mEvictionFuture.get()` at line 1204, gets `eligibleEntries` at line 1215, uses `modifiedKeys` only in filter loop 1217-1242
- `src/util/types.h:18` — `LedgerKeySet` is `std::set<LedgerKey, LedgerEntryIdCmp>` (ordered set, NOT unordered hash set)
- `src/bucket/LedgerCmp.h:55-122` — `LedgerEntryIdCmp` performs field-by-field comparison; for CONTRACT_DATA entries, calls `lexCompare` on `contract`, `key` (SCVal), and `durability`
- `src/ledger/InternalLedgerEntry.h:88` — `ledgerKey()` returns `LedgerKey const&` (no copy on access, but `emplace` copies into the set)

### Findings

**Correction to hypothesis**: `LedgerKeySet` is `std::set<LedgerKey, LedgerEntryIdCmp>` — an **ordered** balanced tree, not an `UnorderedSet` with SipHash. This means:
- Insertion cost is O(log n) comparisons per element, not O(1) amortized hash + insert
- Total construction is O(n log n), not O(n)
- For n=50,000 and log₂(50000)≈16, each insertion requires ~16 comparisons via `LedgerEntryIdCmp`
- For CONTRACT_DATA keys (dominant in SAC/token benchmarks), each comparison involves `lexCompare` over `SCAddress` + `SCVal` fields
- The actual cost may be **higher** than the hypothesis's 4-12ms estimate

**Core claim confirmed**: The set is built unconditionally at line 2962, but the sole consumer (the filter loop at BucketManager.cpp:1217-1242) only executes when `eligibleEntries` is non-empty. The eviction future resolves at line 1204 — the candidate count is known before the set is needed.

**Benchmark impact**: In apply-load benchmarks with `APPLY_LOAD_BL_SIMULATED_LEDGERS=0`, the BucketList is nearly empty and all entries have maximum TTLs — zero eviction candidates guaranteed. The set is 100% wasted work.

**Severity assessment**: Downgraded from Low to Informational. While the per-invocation waste is real and potentially 5-20ms, total ledger close times at TX=6400 are in the hundreds of milliseconds range, making this likely <5% of total throughput. The improvement needs benchmarking to confirm magnitude.

**Implementation note**: Two callers in test code pass different key sets to `resolveBackgroundEvictionScan` — `BucketTestUtils.cpp:222` passes only TTL keys, and `InvariantTests.cpp:407` passes an empty set. The PoC must accommodate these different usage patterns.

### PoC Guidance

- **Target code**: `src/bucket/BucketManager.cpp:resolveBackgroundEvictionScan` and `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges`
- **Change description**: Modify `resolveBackgroundEvictionScan` to lazily construct the key set. Recommended approach: change the `LedgerKeySet const& modifiedKeys` parameter to `std::function<LedgerKeySet()> getModifiedKeys` (or equivalent lazy wrapper). Inside the function, after resolving the eviction future (line 1204) and checking validity (lines 1208-1213), check `eligibleEntries.empty()`. If empty, skip the filter loop entirely without ever invoking the key set factory. If non-empty, call the factory to build the set and proceed with filtering. Update call sites: (1) `LedgerManagerImpl.cpp:2962` — pass a lambda `[&ltx]() { return ltx.getAllKeysWithoutSealing(); }`, (2) `BucketTestUtils.cpp:222` — pass a lambda capturing the locally built key set, (3) `InvariantTests.cpp:407` — pass a lambda returning empty set.
- **Correctness check**: Existing tests covering eviction: `[bucket]` tag tests, InvariantTests eviction tests. Run `./src/stellar-core test --ll fatal -r simple --abort --disable-dots "[bucket]"` and any tests matching `evict` or `Eviction`.
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` for the SAC scenario. Measure `finalizeLedgerTxnChanges` wall time. Expect the `getAllKeysWithoutSealing` cost to disappear entirely in benchmark scenarios with zero eviction candidates.
