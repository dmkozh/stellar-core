# H004: commitChangesToLedgerTxn Uses Expensive load() Per Entry Instead of Bulk Operations

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Medium
**Impact**: CPU reduction in post-parallel-apply commit path, affects all T=1 and T=8 Soroban scenarios
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When committing the results of parallel Soroban transaction execution back into the `LedgerTxn`, the system should use the most efficient available mutation API. Since the parallel apply system already knows the final state of each entry (new value, updated value, or deleted), it should write entries directly without redundantly looking them up from the parent LedgerTxn.

## Mechanism

`GlobalParallelApplyLedgerState::commitChangesToLedgerTxn()` (ParallelApplyUtils.cpp:404-474) iterates all dirty entries in `mGlobalEntryMap` and commits them back to the main `LedgerTxn`. For each dirty entry, it calls `ltxInner.load(key)` (lines 421, 433) which:

1. Checks `mActive` map (hash map lookup)
2. Calls `getNewestVersionEntryMap(key)` which searches the entry map AND the parent chain
3. If found, creates a `shared_ptr<EntryImplBase>` and `LedgerTxnEntry` wrapper object
4. Inserts into `mActive` map (another hash map insert)
5. Calls `updateEntry` to track the loaded entry

This is done **twice** for entries being deleted (once for the `load()` check at line 433, once for the `erase` at line 436). In contrast, `updateWithoutLoading()` (LedgerTxn.cpp:780-797) and `createWithoutLoading()` (LedgerTxn.cpp:750-771) skip all of steps 1-4, going directly to `updateEntry` with the new value. They bypass active tracking entirely.

With thousands of Soroban entries modified per ledger (e.g., SAC benchmark: ~6400 txs × ~4 entries/tx = ~25,600 footprint entries, of which many are dirty), the overhead of `load()` per entry is significant. Each `load()` involves ~3-4 hash map operations + heap allocation for the shared_ptr + LedgerTxnEntry construction, whereas `updateWithoutLoading` involves ~1 hash map operation with no heap allocation.

## Trigger

Run any Soroban apply-load benchmark (sac, custom_token, soroswap) at T=1 or T=8. Profile the `commitChangesToLedgerTxn` Tracy zone. The zone should show per-entry `load()` overhead dominating commit time.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:404-474` — `commitChangesToLedgerTxn` loop doing `load()` per entry
- `src/transactions/ParallelApplyUtils.cpp:421` — `ltxInner.load(key)` for update path
- `src/transactions/ParallelApplyUtils.cpp:433` — `ltxInner.load(key)` for delete path
- `src/ledger/LedgerTxn.cpp:1883-1926` — `LedgerTxn::Impl::load()` showing the expensive path
- `src/ledger/LedgerTxn.cpp:780-797` — `updateWithoutLoading()` showing the cheap path
- `src/ledger/LedgerTxn.cpp:750-771` — `createWithoutLoading()` showing the cheap path

## Evidence

1. `load()` at LedgerTxn.cpp:1883 does: `getNewestVersionEntryMap` → parent chain search → `make_shared<EntryImplBase>` → `mActive.emplace` → `updateEntry`. This is 4-5 operations per entry.
2. `updateWithoutLoading()` at LedgerTxn.cpp:780 does: `mActive.find` (assertion check) → `updateEntry`. This is 2 operations per entry.
3. The `commitChangesToLedgerTxn` function already has all the information needed: it knows whether each entry exists (from `mGlobalEntryMap`), has the final value, and can determine create vs update from whether the entry was in the pre-parallel global state.
4. For entries present in `mGlobalEntryMap` that were loaded from the LedgerTxn via `preParallelApplyAndCollectModifiedClassicEntries`, the parent LedgerTxn already has these entries, so `load()` redundantly re-fetches them.
5. For Soroban entries loaded from `InMemorySorobanState` on worker threads, the parent LedgerTxn does NOT have them, so `load()` will search up the parent chain and not find them, then the code falls through to `create()`. Using `createWithoutLoading()` directly would be faster.
6. The `LedgerTxnEntry` objects created by `load()` are immediately consumed (`.current() = *updatedLe`) and then go out of scope, so the active tracking infrastructure is pure waste.

## Anti-Evidence

1. Using `eraseWithoutLoading` changes the LedgerTxn consistency mode to `EXTRA_DELETES`, which propagates to the parent and affects `getAllEntries` behavior. This could require careful handling or using `load() + erase()` only for deletes.
2. The `load()` approach is the "safe" API — it verifies the entry actually exists before updating. Switching to `*WithoutLoading` APIs removes this safety check. If there's a bug in the parallel apply state, it could lead to silent data corruption instead of a runtime error.
3. The actual savings per entry are small (~1-3μs based on hash map overhead). But with 5,000-25,000 dirty entries, the total savings could be 5-75ms per ledger.
4. The parent chain search in `getNewestVersionEntryMap` may be cheap if the entry is found in the immediate parent's `mEntry` map (O(1) hash lookup). The more expensive case is when the entry doesn't exist and the search goes all the way to `LedgerTxnRoot`.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full `commitChangesToLedgerTxn` path from `applySorobanStages` (LedgerManagerImpl.cpp:2552) through the `LedgerTxn` nesting hierarchy (`LedgerTxnRoot` → `ltx` → `ltxInner`). Confirmed that `load()` performs unnecessary parent chain lookups, heap allocations, active tracking, and a redundant deep copy for every dirty entry. The `*WithoutLoading` APIs skip these operations but require knowing INIT vs LIVE entry state, which the current `ParallelApplyEntry` struct does not track.

### Code Paths Examined

- `src/transactions/ParallelApplyUtils.cpp:404-474` — `commitChangesToLedgerTxn` loop: creates `ltxInner(ltx)`, iterates all dirty entries, calls `load()` per entry. Confirmed the pattern: load→modify→drop for updates, load→erase for deletes.
- `src/ledger/LedgerTxn.cpp:1883-1926` — `LedgerTxn::Impl::load()`: performs `getNewestVersionEntryMap(key)` → `make_shared<InternalLedgerEntry>` (deep copy) → `LedgerTxnEntry::makeSharedImpl` → `mActive.emplace` → `updateEntry`. For Soroban entries not in `ltx.mEntry`, this traverses to `LedgerTxnRoot::getNewestVersion` which queries `InMemorySorobanState`.
- `src/ledger/LedgerTxn.cpp:780-797` — `updateWithoutLoading()`: only does `mActive.find` (assertion) + `updateEntry` with `LedgerEntryPtr::Live(make_shared(...))`. Saves ~4 hash ops, 1 shared_ptr alloc, and 1 deep copy vs `load()`.
- `src/ledger/LedgerTxn.cpp:750-771` — `createWithoutLoading()`: same efficiency as `updateWithoutLoading` but marks entry as INIT.
- `src/ledger/LedgerTxn.cpp:99-129` — `LedgerEntryPtr::mergeFrom()`: CRITICAL — merging child INIT into parent LIVE **throws** (line 117-121). This means `createWithoutLoading` CANNOT be used for entries that already exist as LIVE in `ltx.mEntry`.
- `src/ledger/LedgerTxn.cpp:1626-1667` — `getAllEntries()`: INIT vs LIVE distinction propagates to BucketList batch classification. Getting this wrong would corrupt the BucketList.
- `src/ledger/LedgerTxn.cpp:997-1017` — `eraseWithoutLoading()`: sets `mConsistency = EXTRA_DELETES`, which prevents correct delta/change calculation. Should be avoided.
- `src/transactions/TransactionFrameBase.h:56-79` — `ParallelApplyEntry<S>`: only has `mLedgerEntry` and `mIsDirty`. No field tracking whether entry is newly created vs pre-existing.
- `src/transactions/ParallelApplyUtils.cpp:312-401` — Constructor + `preParallelApplyAndCollectModifiedClassicEntries`: classic entries loaded from `ltx.getNewestVersionBelowRoot()` into `mGlobalEntryMap` with `mIsDirty=false`. These exist in `ltx.mEntry`.
- `src/ledger/LedgerTxn.cpp:3614-3644` — `LedgerTxnRoot::Impl::getNewestVersion`: for Soroban entries, checks cache then `InMemorySorobanState`. This is the terminal lookup in the parent chain for entries not in `ltx.mEntry`.

### Findings

**The inefficiency is real.** Per dirty entry, `load()` performs:
1. `mActive.find(key)` on `ltxInner` — hash lookup (~50ns)
2. `getNewestVersionEntryMap(key)` — `ltxInner.mEntry.find` (miss) + `mParent.getNewestVersion(key)` traversing to `ltx.mEntry` or `LedgerTxnRoot` — 2-3 hash lookups (~100-250ns)
3. `make_shared<InternalLedgerEntry>(*newest.first)` — heap alloc + deep copy (~100-500ns depending on entry size)
4. `LedgerTxnEntry::makeSharedImpl` — shared_ptr alloc (~50ns)
5. `mActive.emplace` — hash insert (~50ns)
6. `updateEntry` — hash emplace into `mEntry` (~50ns)
7. `ltxe.current() = *updatedLe` — **second** deep copy of entry data (~100-500ns)
8. `~LedgerTxnEntry` → `mActive.erase` — hash erase (~50ns)

Total per entry: ~550-1450ns

With `updateWithoutLoading`/`createWithoutLoading`:
1. `mActive.find(key)` — assertion check (~50ns)
2. `make_shared<InternalLedgerEntry>(entry)` — single deep copy (~100-500ns)
3. `updateEntry` — hash emplace (~50ns)

Total per entry: ~200-600ns

**Savings: ~350-850ns per entry.** With ~15,000 dirty entries (SAC benchmark estimate): **~5-13ms per ledger**. Against ~200-500ms ledger close times, this is **~1-6%** improvement.

**Key correctness constraint discovered:** The `INIT` vs `LIVE` entry state distinction is critical for BucketList correctness (affects `getAllEntries` classification) and for `mergeFrom` safety (INIT into LIVE throws). The `ParallelApplyEntry` struct would need a new `mIsNew` boolean to track whether each entry was newly created during parallel apply vs pre-existing.

**Delete path should remain as-is:** Using `eraseWithoutLoading` sets `EXTRA_DELETES` consistency which propagates to the outer `ltx` and can corrupt delta calculations. The `load()+erase()` pattern should be kept for the delete case. Deletes are rare in most Soroban benchmarks, so this doesn't materially affect the savings.

### PoC Guidance

- **Target code**: `src/transactions/ParallelApplyUtils.cpp:404-474` (`commitChangesToLedgerTxn`) and `src/transactions/TransactionFrameBase.h:56-79` (`ParallelApplyEntry<S>`)
- **Change description**:
  1. Add `bool mIsNew = false` to `ParallelApplyEntry<S>` to track whether the entry was newly created (not present in InMemorySorobanState or LedgerTxn before parallel apply)
  2. Set `mIsNew = true` when entries are created during tx execution in `commitChangesFromSuccessfulTx` for keys not previously in the thread entry map
  3. In `commitChangesToLedgerTxn`, for dirty entries with values:
     - If `entry.mIsNew`: use `ltxInner.createWithoutLoading(InternalLedgerEntry(*updatedLe))`
     - If `!entry.mIsNew`: use `ltxInner.updateWithoutLoading(InternalLedgerEntry(*updatedLe))`
  4. Keep `load()+erase()` for the delete path (lines 433-438) to avoid `EXTRA_DELETES`
- **Correctness check**: Run full `[soroban]` tag tests and `[tx]` tag tests. The parallel apply tests in `ParallelSorobanApplyTests` are the primary coverage. Also run `[ledgertxn]` tests for LedgerTxn invariants.
- **Benchmark focus**: Profile the `commitChangesToLedgerTxn` Tracy zone in SAC T=1 and T=8 benchmarks. Expect ~5-13ms reduction per ledger (~1-6% of total close time). The zone should show reduced time per entry.
