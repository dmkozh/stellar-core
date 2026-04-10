# H001: Replace InMemorySorobanState ContractData Polymorphic unordered_set With Flat unordered_map

**Date**: 2025-07-14
**Subsystem**: ledger
**Severity**: Medium (High without H001-reviewed keyhash cache)
**Impact**: CPU — eliminates per-lookup heap allocation, virtual dispatch, and erase/reinsert overhead
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

Lookups, TTL updates, and data modifications in `InMemorySorobanState`'s
ContractData store should operate with O(1) overhead per operation — a single
hash map lookup and direct in-place mutation. No per-operation heap
allocations, no virtual dispatch, and no erase/reinsert cycles should be
necessary for modifying fields of an existing entry.

## Mechanism

The current `mContractDataEntries` is an `unordered_set<InternalContractDataMapEntry>`
that uses a polymorphic class hierarchy (`AbstractEntry` → `ValueEntry`/`QueryKey`)
as a C++17 heterogeneous lookup workaround. This design imposes three categories
of overhead on every operation:

1. **Per-lookup heap allocation**: Every `find()` call constructs a
   `unique_ptr<QueryKey>` (~50ns alloc + ~50ns dealloc per lookup). With ~32K–64K
   lookups per ledger during parallel apply (`getLiveEntryOpt` →
   `InMemorySorobanState::get`), this adds ~3–6ms.

2. **Erase/reinsert for mutations**: Since set elements are immutable,
   `updateContractDataTTL` and `updateContractData` must erase and reinsert the
   entry. Each cycle destroys a `ValueEntry` (heap dealloc) and creates a new one
   (SHA-256 for hash + heap alloc). With ~21K TTL updates + ~21K data updates per
   SAC ledger, this adds ~8–12ms (with H001-reviewed's keyhash cache) or ~40ms+
   (without it).

3. **Virtual dispatch**: Every hash and equality check goes through virtual
   function calls, adding ~5ns per call but degrading branch prediction and
   inlining across ~100K+ total calls per ledger.

A flat `unordered_map<uint256, ContractDataValue>` keyed by the TTL key hash
(which is already the canonical identifier for ContractData entries) would
eliminate all three categories: lookups use direct uint256 hashing, mutations
use `operator[]` or `insert_or_assign` for in-place modification, and there is
no polymorphism.

## Trigger

Run the SAC apply-load benchmark at T=8 with 3200 transactions per ledger
(`APPLY_LOAD_BATCH_SAC_COUNT=100`, `GENESIS_TEST_ACCOUNT_COUNT=21000`).
The overhead is proportional to the number of unique ContractData entries
modified per ledger (~21K for SAC) plus the number of `get()` lookups during
parallel apply (~32K–64K).

## Target Code

- `src/ledger/InMemorySorobanState.h:82-286` — `InternalContractDataMapEntry` class hierarchy (`AbstractEntry`, `ValueEntry`, `QueryKey`)
- `src/ledger/InMemorySorobanState.h:279-285` — `InternalContractDataEntryHash` functor
- `src/ledger/InMemorySorobanState.cpp:53-63` — `updateContractDataTTL()`: erase/reinsert pattern
- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData()`: erase/reinsert pattern
- `src/ledger/InMemorySorobanState.cpp:114-142` — `createContractDataEntry()`: SHA-256 via find + getTTLKey
- `src/ledger/InMemorySorobanState.cpp:204-218` — `get()` for CONTRACT_DATA: QueryKey construction

## Evidence

1. **Measured overhead pattern**: Each `get()` for CONTRACT_DATA constructs a
   `unique_ptr<QueryKey>` (heap alloc) that computes `getTTLKey(ledgerKey)` (SHA-256
   + XDR serialize), then is destroyed immediately after the lookup (heap dealloc).
   This is ~600ns of unnecessary allocation overhead per lookup.

2. **Explicit comment acknowledges C++17 limitation**: The code at
   `InMemorySorobanState.h:83-91` explains the polymorphic pattern is a C++17
   workaround. A flat map with uint256 key achieves the same heterogeneous lookup
   goal without the workaround.

3. **ContractCode already uses flat map**: `mContractCodeEntries` is declared as
   `UnorderedMap<uint256, ContractCodeMapEntry>` (line 316 of InMemorySorobanState.h),
   proving the flat map pattern works for this codebase. Only ContractData uses the
   polymorphic set.

4. **Erase/reinsert comment confirms immutability constraint**: The comment at
   `InMemorySorobanState.cpp:58` says "Since entries are immutable, we must erase
   and re-insert" — this constraint is an artifact of the set design, not a
   fundamental requirement.

5. **High call frequency**: For SAC with 3200 txs × 100 transfers, ~21K unique
   ContractData entries are modified per ledger. Each modification calls
   `updateContractData` (erase/reinsert) plus `updateTTL` → `updateContractDataTTL`
   (another erase/reinsert). That's ~42K erase/reinsert cycles per ledger.

## Anti-Evidence

1. **SHA-256 key derivation cost remains**: Converting a CONTRACT_DATA `LedgerKey`
   to a `uint256` TTL key hash still requires `getTTLKey()` → SHA-256. This ~500ns
   cost per lookup is inherent to the key derivation and is NOT eliminated by the
   flat map. The savings come from removing the ADDITIONAL overhead (heap alloc,
   virtual dispatch, equality-check SHA-256, erase/reinsert).

2. **H001-reviewed partially overlaps**: The reviewed hypothesis H001 (cache
   ValueEntry keyhash) addresses the SHA-256 cost in `hash()` and `copyKey()`.
   If H001 is implemented first, the incremental savings from the flat map are
   smaller (~11ms vs ~47ms without H001). However, the flat map subsumes H001
   entirely and eliminates additional overhead H001 does not address (heap alloc,
   erase/reinsert).

3. **Requires interface refactor**: The `InternalContractDataMapEntry` class
   hierarchy is used across `InMemorySorobanState.h/.cpp` and potentially in
   tests. Replacing it requires defining a new `ContractDataValue` struct and
   updating all callers. The change is localized to the `InMemorySorobanState`
   class but nontrivial.
