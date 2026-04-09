# H001: Cache TTL Key Hash in InternalContractDataMapEntry::ValueEntry

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Low
**Impact**: CPU reduction in Soroban entry lookups during parallel apply
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When looking up a ContractData entry in `InMemorySorobanState` via `mContractDataEntries.find()`, the `unordered_set` should compute the hash of the stored entry in O(1) using a cached value, and equality comparison should use the cached hash directly. The total cost of a lookup should be dominated by the hash table probe, not by cryptographic hashing.

## Mechanism

`InternalContractDataMapEntry::ValueEntry::hash()` calls `copyKey()` which calls `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))`. The `getTTLKey` function (LedgerTypeUtils.cpp:36) computes `sha256(xdr::xdr_to_opaque(e))` — a full SHA-256 hash plus XDR serialization — on **every** call. This means every `unordered_set` operation (find, insert, erase, rehash) on a `ValueEntry` triggers a fresh SHA-256 computation for the stored entry. Furthermore, `operator==` in `AbstractEntry` calls `copyKey()` on **both** operands, so collision resolution during lookups triggers two additional SHA-256 computations per bucket probe.

Note: This hypothesis is complementary to `transaction-ledger/002-cache-getttlkey-sha256` which addresses call-site caching in `ParallelApplyUtils`. This hypothesis targets the **internal data structure** overhead — the `unordered_set` that stores the entire in-memory Soroban state pays SHA-256 costs on every structural operation (insert, erase, rehash during `updateState`), and on equality checks during find. The fix is to cache the `uint256` TTL key hash inside `ValueEntry` at construction time.

## Trigger

Run the apply-load benchmark with any Soroban scenario. Operations on `mContractDataEntries` occur during:
- `InMemorySorobanState::updateState()` — called once per ledger to merge init/live/dead entries
- `InMemorySorobanState::get()` — called per-entry during parallel Soroban apply for entries not in thread/global maps
- `initializeStateFromSnapshot()` — called at startup

## Target Code

- `src/ledger/InMemorySorobanState.h:ValueEntry::copyKey():148-153` — recomputes SHA-256 on every call
- `src/ledger/InMemorySorobanState.h:ValueEntry::hash():155-158` — delegates to copyKey()
- `src/ledger/InMemorySorobanState.h:AbstractEntry::operator==():127-131` — calls copyKey() on both sides
- `src/ledger/LedgerTypeUtils.cpp:31-38` — getTTLKey computes sha256(xdr_to_opaque(e))
- `src/ledger/InMemorySorobanState.cpp:53-63` — updateContractDataTTL erases+reinserts (2 hash computations)
- `src/ledger/InMemorySorobanState.cpp:92-130` — updateContractData calls find (hash computation)

## Evidence

1. `ValueEntry::copyKey()` explicitly calls `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))` which computes `sha256(xdr::xdr_to_opaque(e))`.
2. The `hash()` method delegates to `copyKey()`, confirming no caching exists.
3. `operator==` calls `copyKey()` on both operands — two SHA-256 computations per equality check.
4. `updateContractDataTTL` (line 53-63) does erase+reinsert, paying hash cost twice.
5. The `QueryKey` class already demonstrates the pattern of storing the hash directly — `ValueEntry` should do the same.

## Anti-Evidence

1. `QueryKey` lookups (from `InMemorySorobanState::get`) only trigger `hash()` on the QueryKey side (which has the cached hash) — the `ValueEntry::hash()` is not called during find. Only `operator==` is called on bucket collision, which calls `copyKey()` on the ValueEntry.
2. With good hash distribution, collisions should be rare, so the `operator==` path is rarely hit.
3. The `updateState` path runs once per ledger and is sequential — it's not in the per-tx hot path.
4. If the `InMemorySorobanState::get()` call path is rarely reached (because entries are pre-loaded into thread maps), the impact may be smaller than expected.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced `ValueEntry::hash()` and `copyKey()` through all callers in `InMemorySorobanState.cpp`. Confirmed that `ValueEntry::hash()` recomputes SHA-256 on every call (via `getTTLKey` → `sha256(xdr_to_opaque(e))`). However, `find()` only calls `ValueEntry::hash()` indirectly through `operator==` on successful matches — not through the hash function itself (that uses the QueryKey's cached hash). The per-ledger `updateState()` path triggers ~220–440 SHA-256 computations total across all `find`/`emplace` operations, saving ~0.2–0.4ms per ledger. The invariant snapshot copy (which rehashes all N entries) is gated by `INVARIANT_EXTRA_CHECKS` and disabled in benchmarks.

### Code Paths Examined

- `src/ledger/InMemorySorobanState.h:148-158` — `ValueEntry::copyKey()` and `hash()` confirmed to call `getTTLKey()` → SHA-256 on every invocation
- `src/ledger/InMemorySorobanState.h:127-131` — `AbstractEntry::operator==()` calls `copyKey()` on both operands; triggers SHA-256 on ValueEntry side during every successful `find()`
- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey()` confirmed: `sha256(xdr::xdr_to_opaque(e))` every time
- `src/ledger/InMemorySorobanState.cpp:52-63` — `updateContractDataTTL()`: erase + emplace triggers 1 SHA-256 from emplace (ValueEntry::hash())
- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData()`: find triggers 1 SHA-256 from operator== on match, emplace triggers 1 more
- `src/ledger/InMemorySorobanState.cpp:114-142` — `createContractDataEntry()`: find on non-existent entry (no operator== SHA-256), emplace triggers 1 SHA-256
- `src/ledger/InMemorySorobanState.cpp:372-396` — Copy constructor: emplace for each entry triggers ValueEntry::hash() → SHA-256 per entry
- `src/ledger/LedgerManagerImpl.cpp:778-817` — `maybeRunSnapshotInvariantFromLedgerState()`: copy constructor is gated by `INVARIANT_EXTRA_CHECKS` config flag — disabled in benchmarks
- `src/transactions/ParallelApplyUtils.cpp:700-735` — `getLiveEntryOpt()`: falls through to `mInMemorySorobanState.get()` only when key is absent from `mThreadEntryMap` (pre-populated from footprints, so rarely reached)
- `src/transactions/ParallelApplyUtils.cpp:563-607` — `collectClusterFootprintEntriesFromGlobal()`: pre-loads all footprint keys into thread maps, reducing `InMemorySorobanState::get()` calls to near zero

### Findings

The inefficiency is **real** — `ValueEntry::hash()` and `copyKey()` recompute SHA-256 + XDR serialization on every call, and there is no caching. The proposed fix (cache `uint256` at construction time) is **correct** and doesn't break any invariants since entries are immutable once inserted.

However, the benchmark impact is **minimal** for these reasons:

1. **Per-ledger `updateState()` operations**: The number of `mContractDataEntries` operations per ledger is proportional to the transaction count (~100–300 entries changed), not the total state size. Each operation triggers 1–2 SHA-256 calls. At ~1μs per SHA-256+XDR serialization, total per-ledger cost is ~220–440μs. For a 200ms ledger close, this is ~0.1–0.2%.

2. **Parallel apply `get()` path**: `InMemorySorobanState::get()` is only reached when a key is missing from both thread and global entry maps. Since `collectClusterFootprintEntriesFromGlobal()` pre-loads all footprint keys, the `get()` path is almost never hit during normal Soroban execution.

3. **Rehash during steady-state**: The set size is stable during `updateState()` (creates ≈ deletes), so rehash events are extremely rare.

4. **Invariant snapshot copy**: Happens every ledger but is gated by `INVARIANT_EXTRA_CHECKS`, which is disabled in benchmarks. When enabled, this would save N SHA-256 computations (significant for large N), but it's not in the benchmark path.

5. **Initialization**: The copy saves ~2N SHA-256 from rehash during `initializeStateFromSnapshot()`, but this is a one-time startup cost.

Downgrading severity from **Low** to **Informational**: the improvement is real but too small to measure in apply-load benchmarks (<1% of ledger close time).

### PoC Guidance

- **Target code**: `src/ledger/InMemorySorobanState.h` — `ValueEntry` class (lines 136-174)
- **Change description**: Add `uint256 mCachedKeyHash` member to `ValueEntry`, computed once in the constructor via `getTTLKey(LedgerEntryKey(*entry.ledgerEntry)).ttl().keyHash`. Update `copyKey()` to return `mCachedKeyHash` and `hash()` to return `std::hash<uint256>{}(mCachedKeyHash)`. Update `clone()` to propagate the cached hash. Memory cost: 32 bytes per entry.
- **Correctness check**: Existing tests for `InMemorySorobanState` (search for `InMemorySorobanState` in test files, particularly `[soroban]` tagged tests) should pass unchanged since the behavior is identical — only internal implementation detail changes.
- **Benchmark focus**: Per-ledger `updateState()` time. Expected improvement: <1% (Informational). The invariant snapshot copy (if `INVARIANT_EXTRA_CHECKS` is enabled) would see a more noticeable improvement proportional to total contract data entry count.
