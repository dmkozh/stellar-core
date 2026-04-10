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
