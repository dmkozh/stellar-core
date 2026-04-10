# H001: Eliminate Redundant LedgerEntry Deep Copies in finalizeLedgerTxnChanges → addLiveBatch

**Date**: 2025-07-22
**Subsystem**: transaction-ledger (LedgerManagerImpl, LiveBucket)
**Severity**: Low
**Impact**: Reduced memory allocation overhead in ledger close path
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When committing a ledger close, the entries extracted from the LedgerTxn via
`getAllEntries` should be transferred into the BucketList with minimal copying.
Ideally, each `LedgerEntry` is deep-copied exactly once (from the LedgerTxn
entry map into the output vector) and then moved through subsequent consumers.

## Mechanism

`finalizeLedgerTxnChanges` calls `getAllEntries` which deep-copies all ~16,000
modified entries (for a 3200-tx Soroban ledger) from the LedgerTxn entry map
into `initEntries`, `liveEntries`, and `deadEntries` vectors. These vectors are
then passed as `const&` to three consumers in order:

1. `addAnyContractsToModuleCache` — reads CONTRACT_CODE entries only
2. `addLiveBatch` → `convertToBucketEntry` — deep-copies every entry AGAIN
   into `BucketEntry` objects (`ce.liveEntry() = e` on lines 394, 402)
3. `updateInMemorySorobanState` — reads entries, copies Soroban entries into
   in-memory maps

The second deep copy in `convertToBucketEntry` is unnecessary. By reordering
the calls so that `addLiveBatch` is called last, the entry vectors can be
passed by rvalue reference (`std::move`), allowing `convertToBucketEntry` to
use `ce.liveEntry() = std::move(e)` instead of copying. This eliminates ~16,000
heap allocations for XDR fields (xdr::xvector in SCVal keys/values, opaque
data, etc.).

## Trigger

Any Soroban-heavy ledger close. In the apply-load benchmark:
- 3200 SAC transfer txs × ~5 modified entries each = ~16,000 entries
- Each entry contains XDR union fields with heap-allocated vectors
- `convertToBucketEntry` copies all of them, then sorts

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:3039-3046` — call ordering of addLiveBatch vs updateInMemorySorobanState
- `src/bucket/LiveBucket.cpp:convertToBucketEntry:380-420` — the copy loop (`ce.liveEntry() = e` on lines 394, 402)
- `src/bucket/LiveBucket.cpp:freshInMemoryOnly:467-498` — calls convertToBucketEntry
- `src/bucket/BucketManager.cpp:addLiveBatch:1026-1046` — entry point, takes `const&`

## Evidence

1. `convertToBucketEntry` explicitly copies every entry: `ce.liveEntry() = e` (lines 394, 402) — this is a deep copy of the LedgerEntry XDR including all nested xdr::xvector fields.
2. After `addLiveBatch` returns, the entry vectors are only used by `updateInMemorySorobanState` (line 3045). If that call is moved before `addLiveBatch`, the vectors are free to be moved.
3. `addAnyContractsToModuleCache` (lines 3041-3042) only reads CONTRACT_CODE entries and doesn't modify the vectors, so it can remain before both.
4. The three consumers (`addAnyContractsToModuleCache`, `addLiveBatch`, `updateInMemorySorobanState`) are independent — no data flows between them, no ordering constraints.

## Anti-Evidence

1. For small entries (TTL ~40 bytes, accounts ~200 bytes), the copy cost is dominated by fixed overhead (~50ns per allocation), not data size. Total savings may be ~1-3ms.
2. The sort in `convertToBucketEntry` (line 413) dominates the function time for large entry counts, and moving vs copying doesn't affect sort cost.
3. `updateInMemorySorobanState` also copies entries into its internal maps, so one copy in the chain is unavoidable regardless.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete path from `finalizeLedgerTxnChanges` (LedgerManagerImpl.cpp:2942-3048) through `getAllEntries` → `addAnyContractsToModuleCache` → `addLiveBatch` → `addBatch` → `addBatchInternal` → `prepareFirstLevel` → `freshInMemoryOnly` → `convertToBucketEntry`. Confirmed the deep copy exists at lines 394 and 402 of LiveBucket.cpp. Verified all three consumers are independent with no ordering constraints. The vectors are local to `finalizeLedgerTxnChanges` and destroyed after all consumers return.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:3039-3046` — Confirmed three independent consumers of the entry vectors; vectors are local variables destroyed at function return
- `src/bucket/LiveBucket.cpp:convertToBucketEntry:379-420` — Confirmed deep copy via `ce.liveEntry() = e` for each init/live entry; iterates by `const&`
- `src/bucket/LiveBucket.cpp:freshInMemoryOnly:466-498` — Passes entries through to `convertToBucketEntry` by `const&`
- `src/bucket/BucketListBase.cpp:prepareFirstLevel:196-238` — Two paths: `fresh` (on-disk, line 217) and `freshInMemoryOnly` (in-memory, line 229), both take entries by `const&`
- `src/bucket/BucketManager.cpp:addLiveBatch:1025-1046` — Takes entries by `const&`, passes to `mLiveBucketList->addBatch`
- `src/ledger/LedgerManagerImpl.cpp:ApplyState::addAnyContractsToModuleCache:3148-3176` — Read-only: only looks at CONTRACT_CODE entries for Wasm compilation
- `src/ledger/InMemorySorobanState.cpp:updateState:536-600` — Reads entries by `const&`, copies Soroban entries into internal maps (shared_ptr + emplace)
- `src/ledger/LedgerManagerImpl.cpp:ApplyState::updateInMemorySorobanState:308-318` — Thin wrapper, passes through to `InMemorySorobanState::updateState`

### Findings

**The inefficiency is real**: `convertToBucketEntry` performs an unnecessary deep copy of every `LedgerEntry` into `BucketEntry` objects. Since the entry vectors are local to `finalizeLedgerTxnChanges` and consumed last by `addLiveBatch`, move semantics could eliminate these copies.

**The three consumers are truly independent**: `addAnyContractsToModuleCache` only reads CONTRACT_CODE entries (no mutation). `updateInMemorySorobanState` copies Soroban entries into its own internal maps (doesn't read from BucketList). `addLiveBatch` updates the BucketList (doesn't read from InMemorySorobanState). No data flows between them and no ordering is required.

**Severity downgrade to Informational**: The hypothesis claims "Low" (5-10%) but the actual impact is sub-1% of ledger close time:
- TTL entries (~50% of entries) are ~40 bytes with no heap allocations — copy vs move is nearly free (memcpy-equivalent)
- CONTRACT_DATA entries with SCVal keys have 3-4 heap allocations each (~50ns per allocation saved)
- Estimated ~8,000 entries with significant heap data × 3-4 allocs × 50ns = 1.2-1.6ms savings
- The sort at line 413 (O(n log n) comparisons) dominates `convertToBucketEntry` time and is unaffected by move vs copy
- Ledger close for 3200 SAC benchmark takes 1-2+ seconds, so 1-2ms is <0.2% improvement
- Falls well below the 5% threshold for "Low" severity

**Implementation complexity is higher than suggested**: Changing `addLiveBatch` to accept vectors by value requires propagating the signature change through 6+ functions: `BucketManager::addLiveBatch` → `LiveBucketList::addBatch` → `BucketListBase::addBatchInternal` (template) → `BucketLevel::prepareFirstLevel` (template) → `LiveBucket::freshInMemoryOnly` / `LiveBucket::fresh` → `LiveBucket::convertToBucketEntry`. The template-heavy bucket list code makes this non-trivial.

**Correctness is preserved**: All operations happen on the same apply thread (no thread safety concern). The vectors are local with no external references. `updateInMemorySorobanState` copies entries into shared_ptrs, so it doesn't hold references to the vectors after returning.

### PoC Guidance

- **Target code**: `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges` (reorder calls, change `addLiveBatch` to take vectors by value), `src/bucket/LiveBucket.cpp:convertToBucketEntry` (change to take vectors by value, use `std::move(e)` in assignments), plus signature changes through `BucketManager::addLiveBatch`, `LiveBucketList::addBatch`, `BucketListBase::addBatchInternal`, `BucketLevel::prepareFirstLevel`, `LiveBucket::freshInMemoryOnly`, `LiveBucket::fresh`
- **Change description**: (1) Swap order of `updateInMemorySorobanState` and `addLiveBatch` in `finalizeLedgerTxnChanges`. (2) Change `addLiveBatch` chain to take vectors by value. (3) Use `std::move` in `convertToBucketEntry` assignments. (4) Pass vectors with `std::move` at the call site.
- **Correctness check**: Existing bucket list tests (`[bucket]` tag), ledger close tests, and the full test suite should pass since the optimization is semantically identical
- **Benchmark focus**: Measure `convertToBucketEntry` time specifically (Tracy/ZoneScoped) on 3200 SAC scenario; expect ~1-2ms improvement in that function, negligible impact on overall ledger close time
