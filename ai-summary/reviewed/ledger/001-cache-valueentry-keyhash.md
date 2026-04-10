# H001: Cache TTL Key Hash in InternalContractDataMapEntry ValueEntry

**Date**: 2025-07-14
**Subsystem**: ledger
**Severity**: Medium
**Impact**: Serial + parallel apply throughput (10-20% improvement at T=8)
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

Hash and equality operations on `InternalContractDataMapEntry` instances stored
in `mContractDataEntries` should be O(1) operations that retrieve a
pre-computed hash value, similar to how `QueryKey` stores `ledgerKeyHash`
directly and returns it from `copyKey()` without recomputation.

## Mechanism

`ValueEntry::copyKey()` (InMemorySorobanState.h:148-153) recomputes
`getTTLKey(LedgerEntryKey(*entry.ledgerEntry))` on EVERY invocation. This
calls `sha256(xdr::xdr_to_opaque(e))` — a full SHA-256 hash of the serialized
LedgerKey. This method is called from `hash()` (every set insertion, rehash)
and `operator==` (every equality comparison during find/insert). Since
`mContractDataEntries` is an `unordered_set`, every `find()`, `emplace()`, and
`erase()` on stored ValueEntries triggers one or more SHA-256 computations.

This affects both the serial and parallel portions of the apply path:

**Serial path (updateState after parallel apply):**
- `updateContractData` (InMemorySorobanState.cpp:92-111): find() + erase() +
  emplace() per entry = ~2 SHA-256 per call × ~6,400 calls (SAC benchmark) =
  ~12,800 SHA-256
- `updateTTL` and `createTTL` that search `mContractDataEntries`: ~6,400
  additional SHA-256
- Total serial: ~19,200 SHA-256 at ~1μs each = ~19ms

**Parallel path (worker thread lookups via InMemorySorobanState):**
- `collectClusterFootprintEntriesFromGlobal` → `InMemorySorobanState::get()` →
  find in mContractDataEntries: ~1 SHA-256 per CONTRACT_DATA lookup
- `getLiveEntryOpt` fallthrough → `InMemorySorobanState::get()`/`getTTL()` →
  find in mContractDataEntries: ~1 SHA-256 per CONTRACT_DATA lookup
- Total parallel: ~11,000 SHA-256 across 8 threads = ~1.4ms wall time

**Combined estimate**: ~20-25ms per ledger close (serial dominates), which is
~10-17% of T=8 SAC benchmark close time (~150-250ms).

## Trigger

Run SAC benchmark at T=8 with 3200 transactions. Observe SHA-256 time
attribution in profiler. The `sha256` calls will show up inside
`InternalContractDataMapEntry` operations (hash/equality) during the serial
`updateState` phase and throughout parallel worker thread lookups.

## Target Code

- `src/ledger/InMemorySorobanState.h:148-158` — `ValueEntry::copyKey()` and
  `hash()` recompute SHA-256 on every call
- `src/ledger/InMemorySorobanState.h:127-131` — `AbstractEntry::operator==()`
  calls `copyKey()` on BOTH sides (SHA-256 on the ValueEntry side)
- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData` does
  find + erase + emplace (3 set operations = ~2 SHA-256 per update)
- `src/ledger/InMemorySorobanState.cpp:410-443` — `getTTL` searches
  mContractDataEntries (SHA-256 on equality check)
- `src/ledger/LedgerTypeUtils.cpp:30-38` — `getTTLKey` performs SHA-256

## Evidence

1. `ValueEntry::copyKey()` at line 148-153 calls `getTTLKey(LedgerEntryKey(...))`
   every time, with NO caching of the result. The `QueryKey` class (line 177-213)
   already demonstrates the correct pattern: store the hash once at construction
   and return it from `copyKey()`.

2. The SAC benchmark with 3200 txs produces ~6,400 CONTRACT_DATA updates per
   ledger in `updateState`, each triggering find + erase + emplace = ~19,200
   SHA-256 calls just in the serial update path.

3. The code comment at line 82-96 explains the design rationale (save memory by
   not storing keys twice), but doesn't acknowledge the CPU cost of recomputing
   SHA-256 on every set operation.

4. `ContractCodeMapEntryT` uses `unordered_map<uint256, ...>` (line 323) which
   stores the hash key directly and avoids this problem entirely — demonstrating
   the fix pattern.

## Anti-Evidence

1. The memory overhead of caching the hash is 32 bytes per entry (uint256).
   For ~100K CONTRACT_DATA entries, that's ~3.2MB — negligible compared to the
   entries themselves.

2. The design was intentionally chosen for memory efficiency (line 82-96
   comment). However, the CPU cost may not have been considered at the time of
   implementation, especially in the context of the parallel apply benchmark
   where this code path is hot.

3. Hardware SHA-256 support on modern CPUs reduces the per-call cost to ~0.7μs.
   On such hardware, the impact would be at the low end (~14ms total, ~7-9%).
   On CPUs without SHA-NI, the impact is higher (~40ms+).

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated in fail/ or success/

### Trace Summary

Traced the full `ValueEntry::copyKey()` → `getTTLKey()` → `sha256(xdr_to_opaque(e))` path and confirmed the SHA-256 + XDR serialization is recomputed on every invocation. Verified that `unordered_set::find(QueryKey)` uses QueryKey's cached hash for bucket lookup (O(1)) but calls `ValueEntry::copyKey()` (SHA-256) during `operator==` on bucket matches. The `emplace(ValueEntry)` path calls `ValueEntry::hash()` → SHA-256, but caching merely moves this cost to the constructor (no net savings). The real savings come from eliminating repeated `operator==` SHA-256 calls on stored entries during find matches. Parallel path hits `InMemorySorobanState::get()` for all Soroban entries not in the global map (which only stores classic entries), so every unique Soroban key per cluster triggers at least one find match in `mContractDataEntries`.

### Code Paths Examined

- `src/ledger/InMemorySorobanState.h:148-153` — `ValueEntry::copyKey()`: confirmed calls `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))` on every invocation, no caching
- `src/ledger/InMemorySorobanState.h:155-158` — `ValueEntry::hash()`: delegates to `copyKey()`, so SHA-256 per hash() call
- `src/ledger/InMemorySorobanState.h:127-131` — `AbstractEntry::operator==()`: calls `copyKey()` on both operands; for QueryKey==ValueEntry comparisons, ValueEntry side pays SHA-256
- `src/ledger/InMemorySorobanState.h:177-213` — `QueryKey`: already caches hash in constructor, demonstrating the correct pattern
- `src/ledger/LedgerTypeUtils.cpp:30-38` — `getTTLKey()`: `sha256(xdr::xdr_to_opaque(e))` confirmed
- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData()`: find(QueryKey) → 1 SHA-256 from operator== match + emplace(ValueEntry) → 1 SHA-256 from hash() (moved to constructor with fix, 0 net savings) = 1 SHA-256 saved per call
- `src/ledger/InMemorySorobanState.cpp:52-63` — `updateContractDataTTL()`: erase(iter) + emplace(ValueEntry) → 1 SHA-256 from hash() (moved to constructor, 0 net savings)
- `src/ledger/InMemorySorobanState.cpp:66-89` — `updateTTL()`: find(QueryKey) in mContractDataEntries → 1 SHA-256 from operator== match + calls updateContractDataTTL = 1 SHA-256 saved per call
- `src/ledger/InMemorySorobanState.cpp:204-236` — `get()`: find(QueryKey) for CONTRACT_DATA → 1 SHA-256 from operator== on match → 1 SHA-256 saved per call
- `src/ledger/InMemorySorobanState.cpp:410-443` — `getTTL()`: find(QueryKey) in mContractDataEntries → 1 SHA-256 from operator== on match → 1 SHA-256 saved per call
- `src/transactions/ParallelApplyUtils.cpp:324-386` — `preParallelApplyAndCollectModifiedClassicEntries()`: global map only stores classic entries (skips `isSorobanEntry` at line 337-340), so Soroban entries are NOT pre-loaded
- `src/transactions/ParallelApplyUtils.cpp:563-607` — `collectClusterFootprintEntriesFromGlobal()`: only loads from global map; Soroban entries missing → thread map starts empty for Soroban keys
- `src/transactions/ParallelApplyUtils.cpp:699-735` — `getLiveEntryOpt()`: falls through to `InMemorySorobanState::get()` for all Soroban keys not in mThreadEntryMap

### Findings

The inefficiency is **real** and the fix is **correct**. Key findings:

1. **Net savings are from operator== only.** Caching the hash in the ValueEntry constructor moves the SHA-256 from `hash()` to construction time — no net savings on emplace. The real savings come from `operator==` during `find()` matches, where the stored ValueEntry's `copyKey()` currently recomputes SHA-256 but would return the cached value after the fix.

2. **Per-ledger savings (serial updateState):** For SAC at 3200 txs:
   - ~6,400 `updateContractData` calls × 1 SHA-256 saved = 6,400 saved
   - ~6,400 `updateTTL` calls × 1 SHA-256 saved = 6,400 saved
   - Total: ~12,800 SHA-256 saved × ~1μs = **~12.8ms**

3. **Parallel path savings:** Global entry map excludes Soroban entries, so `getLiveEntryOpt()` falls through to `InMemorySorobanState::get()` for every unique Soroban key per cluster. With ~800 unique CONTRACT_DATA keys + ~800 TTL lookups per cluster at T=8: ~1,600 SHA-256 saved per cluster. Wall time: ~1.6ms.

4. **Total combined savings: ~14.4ms** on ~200ms ledger close at T=8 = **~7.2%**. This fits Low severity (5-10%).

5. **Hypothesis severity correction:** The hypothesis claims Medium (10-20%) based on ~19,200 SHA-256 total serial savings. However, the emplace SHA-256 cost is merely moved to the constructor (not eliminated), reducing actual savings to ~12,800 SHA-256. On CPUs with SHA-NI hardware, this yields ~7% improvement (Low). On CPUs without SHA-NI, the improvement could reach ~15% (Medium).

6. **Memory overhead:** 32 bytes (uint256) per entry. For 100K entries: ~3.2MB — negligible.

7. **Correctness:** Entries in `mContractDataEntries` are immutable after insertion (mutations go through erase+reinsert). Caching the hash at construction is safe.

### PoC Guidance

- **Target code**: `src/ledger/InMemorySorobanState.h` — `ValueEntry` class (lines 136-174)
- **Change description**: Add `uint256 mCachedKeyHash` member to `ValueEntry`, computed once in the constructor via `getTTLKey(LedgerEntryKey(*entry.ledgerEntry)).ttl().keyHash`. Update `copyKey()` to return `mCachedKeyHash` directly. Update `hash()` to return `std::hash<uint256>{}(mCachedKeyHash)`. Update `clone()` to propagate the cached hash (either recompute from the cloned entry, or pass the cached hash through a private constructor). Memory cost: 32 bytes per entry.
- **Correctness check**: Existing tests tagged `[soroban]` cover `InMemorySorobanState` indirectly through Soroban tx execution. The `InMemorySorobanState` is also exercised via `ApplyLoad` benchmark paths. Run `[soroban]` tests to verify no regressions.
- **Benchmark focus**: Run SAC benchmark at T=8 with 3200 txs. Measure total ledger close time. Expected improvement: ~5-10% (Low severity). The serial `updateState` phase should show the most improvement. Profile SHA-256 call count before/after to verify elimination.
