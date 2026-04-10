# H002: Cache TTL Key Hash in InMemorySorobanState

**Date**: 2025-07-14
**Subsystem**: transaction-ledger
**Severity**: Low
**Impact**: 4-7% reduction across scenarios (up to 10% for custom_token T=8)
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `InMemorySorobanState::updateState` processes ~12800 entry updates per
ledger (6400 CONTRACT_DATA + 6400 TTL for SAC), each hash table operation
should use a cached key hash for bucket placement and equality comparison.
The SHA256-based TTL key hash (`getTTLKey`) should be computed once per entry
at insertion time and reused for all subsequent lookups, not recomputed on
every `hash()` and `operator==` call.

## Mechanism

`InternalContractDataMapEntry::ValueEntry` stores entries in an
`unordered_set`. The `hash()` and `operator==` virtual methods both call
`copyKey()`, which recomputes `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))`
on every invocation. `getTTLKey` performs:

1. `LedgerEntryKey(entry)` — constructs a LedgerKey (~100ns)
2. `xdr::xdr_to_opaque(key)` — serializes key to a new `vector<uint8_t>` (~200ns allocation + serialize)
3. `sha256(...)` — SHA256 hash of ~100-150 bytes (~500ns)

Total: **~800ns per `copyKey()` call**.

For `updateContractData` (the hot path), each call triggers:
- `find()`: 1 SHA256 for QueryKey construction + 1 SHA256 for ValueEntry comparison = 2
- `erase()`: 0 (iterator-based)
- `emplace()`: 1 SHA256 for ValueEntry::hash() + 0-1 for comparison = 1-2
- Total: **3-4 SHA256 per CONTRACT_DATA update**

For `updateTTL`:
- `find()`: 0 SHA256 for QueryKey (TTL keys store hash directly) + 1 SHA256 for comparison = 1
- `updateContractDataTTL` → `erase()` + `emplace()`: 1 SHA256 for hash = 1
- Total: **2 SHA256 per TTL update**

For 6400 CONTRACT_DATA updates + 6400 TTL updates per ledger:
- CONTRACT_DATA: 6400 × 3 × 800ns = **15.4ms**
- TTL: 6400 × 2 × 800ns = **10.2ms**
- Total: **~25.6ms** (4.2% of 612ms SAC T=8)

For custom_token (3000 txs × ~6 entries × 3 SHA256 × 800ns):
- Total: **~43ms** (10% of 430ms custom_token T=8)

The fix: add a `uint256 mCachedKeyHash` field to `ValueEntry`, populated once
in the constructor from `getTTLKey()`. Then `copyKey()` returns the cached
value and `hash()` uses `std::hash<uint256>{}(mCachedKeyHash)`.

## Trigger

Run apply-load benchmark with custom_token scenario at TX=3000, T=8.
Profile `InMemorySorobanState::updateState` and its callees. The SHA256
recomputation will show up in `getTTLKey` → `sha256` → crypto primitives,
called from `InternalContractDataMapEntry::ValueEntry::hash()` and
`InternalContractDataMapEntry::ValueEntry::copyKey()`.

## Target Code

- `src/ledger/InMemorySorobanState.h:136-174` — `ValueEntry` class with
  `copyKey()` and `hash()` virtual methods that recompute SHA256
- `src/ledger/InMemorySorobanState.h:225-230` — constructor that could
  pre-compute the hash
- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData`:
  find + erase + emplace cycle triggering 3-4 SHA256 calls
- `src/ledger/InMemorySorobanState.cpp:53-63` — `updateContractDataTTL`:
  erase + emplace cycle triggering 1-2 SHA256 calls
- `src/ledger/InMemorySorobanState.cpp:66-89` — `updateTTL`: find +
  updateContractDataTTL triggering 2-3 SHA256 calls
- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey()`: the expensive
  `sha256(xdr::xdr_to_opaque(e))` call

## Evidence

1. `ValueEntry::copyKey()` at line 149-153 explicitly calls
   `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))` — no caching.
2. `ValueEntry::hash()` at line 156-158 calls `copyKey()` — recomputes SHA256.
3. The code comment at line 82-96 explains the design uses `unordered_set`
   for memory efficiency (avoiding duplicate key storage). The hash is
   computed from the entry data, not stored separately.
4. Each `updateContractData` call does `erase` + `emplace` (lines 108-110),
   which reconstructs the ValueEntry from scratch — the hash computed during
   the prior insertion is discarded.
5. `xdr_to_opaque` allocates a new vector on every call — no reuse.

## Anti-Evidence

1. **Memory overhead.** Adding a 32-byte `uint256` to each `ValueEntry`
   increases per-entry memory by 32 bytes. For ~50K entries in-memory, that's
   ~1.6MB. Negligible.
2. **Correctness risk.** The cached hash must match the entry's key. Since
   keys are immutable after construction (the entry is erased and re-created
   on update), the cache is always valid within a ValueEntry's lifetime.
3. **The `updateContractData` erase+emplace pattern** means the cached hash
   in the old ValueEntry is destroyed and must be recomputed for the new one.
   However, since we're creating from the same LedgerEntry (with updated
   data but same key), we could pass the old hash to the new ValueEntry
   to avoid even this one recomputation.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated. Fail 010 attempted SHA256 caching via an external `UnorderedMap<LedgerKey, LedgerKey>` whose own hash/equality overhead exceeded the savings. This hypothesis uses an inline `uint256` field inside `ValueEntry` with zero lookup cost, which is a fundamentally different approach.

### Trace Summary

Traced the complete `updateState` → `updateContractData` / `updateTTL` → `ValueEntry::copyKey()` → `getTTLKey()` → `sha256(xdr::xdr_to_opaque(...))` chain. Confirmed that `ValueEntry::copyKey()` recomputes SHA256 on every invocation (lines 148-153 of InMemorySorobanState.h), and that both `hash()` and `operator==` call `copyKey()`. Each `updateContractData` call triggers 3 SHA256 computations (1 for QueryKey construction, 1 for find equality, 1 for emplace hash). Each `updateTTL` (ContractData path) triggers 2 SHA256 computations (1 for find equality, 1 for emplace hash). The `updateState` function runs sequentially on the primary apply thread after all transactions are applied, so these costs are not parallelized.

### Code Paths Examined

- `src/ledger/InMemorySorobanState.h:148-158` — `ValueEntry::copyKey()` and `ValueEntry::hash()` both recompute `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))` on every call. Confirmed no caching.
- `src/ledger/InMemorySorobanState.h:127-131` — `AbstractEntry::operator==` calls `copyKey()` on both operands, triggering SHA256 on both ValueEntry sides (though QueryKey caches its hash).
- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey(LedgerKey)` performs `sha256(xdr::xdr_to_opaque(e))` — the expensive operation. Each call allocates a new `vector<uint8_t>` for serialization, then computes SHA256.
- `src/ledger/InMemorySorobanState.cpp:91-111` — `updateContractData`: `find(QueryKey)` triggers 1 SHA256 for QueryKey + 1 SHA256 for ValueEntry equality; `erase(iterator)` is free; `emplace(new ValueEntry)` triggers 1 SHA256 for hash. Total: 3 SHA256.
- `src/ledger/InMemorySorobanState.cpp:53-63` — `updateContractDataTTL`: `erase(iterator)` is free; `emplace(new ValueEntry with old ledgerEntry)` triggers 1 SHA256 for hash. Total: 1 SHA256.
- `src/ledger/InMemorySorobanState.cpp:65-89` — `updateTTL` (ContractData path): QueryKey from TTL key needs 0 SHA256; find equality triggers 1 SHA256 on ValueEntry; delegates to `updateContractDataTTL` which adds 1 SHA256. Total: 2 SHA256.
- `src/ledger/InMemorySorobanState.cpp:536-602` — `updateState`: iterates all `initEntries`, `liveEntries`, `deadEntries`, dispatching to the above methods. Runs on primary apply thread (confirmed via `LedgerManagerImpl::ApplyState::updateInMemorySorobanState` at LedgerManagerImpl.cpp:315).
- `src/ledger/LedgerTxn.cpp:3639-3641` — `LedgerTxnRoot::Impl::getNewestVersion` calls `mInMemorySorobanState.get()` for Soroban lookups, which also triggers SHA256 via `find()` equality (1 SHA256 per CONTRACT_DATA lookup).

### Findings

**The inefficiency is confirmed.** Every `unordered_set` operation on `mContractDataEntries` that involves a ValueEntry triggers SHA256 recomputation through `copyKey()`. The operations affected are: `find()` (equality comparison), `emplace()` (hashing), and `operator==` (both sides). There is no caching whatsoever — the hash is recomputed from scratch each time.

**SHA256 call count per ledger (SAC TX=6400 estimate):**
- ~6400 CONTRACT_DATA updates × 3 SHA256 = 19,200 SHA256 calls
- ~6400 TTL updates (ContractData path) × 2 SHA256 = 12,800 SHA256 calls
- Total: ~32,000 SHA256 calls at ~600-800ns each = 19-26ms

**SHA256 call count per ledger (custom_token TX=3000 estimate):**
- ~9000 CONTRACT_DATA updates × 3 SHA256 = 27,000 SHA256 calls
- ~9000 TTL updates × 2 SHA256 = 18,000 SHA256 calls
- Total: ~45,000 SHA256 calls at ~600-800ns each = 27-36ms

**With full caching (inline field + hash propagation on erase+emplace):**
- CONTRACT_DATA updates: 1 SHA256 per call (just QueryKey construction), save 2/call
- TTL updates: 0 SHA256 per call (TTL key hash available directly), save 2/call
- SAC: save ~25,600 SHA256 calls = 15-20ms (2.5-3.3% of 612ms)
- custom_token: save ~36,000 SHA256 calls = 22-29ms (5-7% of 430ms)

**Novelty vs fail 010:** Fail 010 added an `unordered_map<LedgerKey, LedgerKey>` cache at call sites. This failed because hashing `LedgerKey` for the cache's own hash table was nearly as expensive as the SHA256 it cached. H002 stores the hash as an inline `uint256` field in `ValueEntry` — zero lookup cost, zero extra hashing, just 32 bytes of memory per entry. This is the correct approach to this problem.

**Correctness is safe.** ValueEntry instances are immutable in the `unordered_set` (entries are erased and re-emplaced on update). The cached hash remains valid for the lifetime of each ValueEntry instance. The key (CONTRACT_DATA LedgerKey) never changes within a ValueEntry.

**Impact assessment:** The custom_token T=8 scenario likely reaches the 5% Low threshold. SAC T=8 is below 5% (~3%). The `get()` path in `LedgerTxnRoot` adds additional SHA256 savings for read operations, but these are harder to quantify without profiling.

### PoC Guidance

- **Target code**: `src/ledger/InMemorySorobanState.h` — `ValueEntry` class (lines 136-174) and constructors (lines 224-237)
- **Change description**:
  1. Add `uint256 mCachedKeyHash` field to `ValueEntry`.
  2. In the `ValueEntry` constructor (line 142-146), compute `mCachedKeyHash = getTTLKey(LedgerEntryKey(*entry.ledgerEntry)).ttl().keyHash`.
  3. Change `copyKey()` to `return mCachedKeyHash;` (was: recompute SHA256).
  4. Change `hash()` to `return std::hash<uint256>{}(mCachedKeyHash);`.
  5. In `clone()`, propagate the cached hash to the new ValueEntry (add a private constructor that accepts `uint256 cachedHash`).
  6. **Bonus optimization**: In `updateContractData` and `updateContractDataTTL`, extract the hash from the old entry before erasing, and pass it to the new ValueEntry constructor (add an overload that accepts a pre-computed `uint256` keyHash). This eliminates the 1 remaining SHA256 per update.
- **Correctness check**: Existing tests via `[soroban]` tag, plus InMemorySorobanState-specific tests if any. The `updateState` path is exercised by any Soroban transaction test.
- **Benchmark focus**: custom_token TX=3000, T=8 (highest entry-per-tx ratio). Measure `updateState` wall time and total ledger close time. Expected improvement: 5-7% reduction in close time for custom_token, 2-3% for SAC.
