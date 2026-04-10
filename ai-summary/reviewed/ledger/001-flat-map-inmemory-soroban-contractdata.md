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

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated. Related hypotheses exist (reviewed/001-cache-valueentry-keyhash caches SHA-256 hash; fail/019 is a duplicate of reviewed/001; fail/023 proposes cross-pipeline caching and was rejected as too invasive). This hypothesis proposes a fundamentally different structural change (replace the data structure entirely) that subsumes the keyhash cache approach.

### Trace Summary

Traced the complete execution path from parallel apply through `ThreadParallelApplyLedgerState::getLiveEntryOpt()` → `InMemorySorobanState::get()` → `mContractDataEntries.find(InternalContractDataMapEntry(ledgerKey))`, confirming that every CONTRACT_DATA lookup constructs a `unique_ptr<QueryKey>` (heap allocation), computes SHA-256 via `getTTLKey()`, and triggers a second SHA-256 via `ValueEntry::copyKey()` on equality match. Confirmed the serial `updateState()` path does erase/reinsert for every CONTRACT_DATA modification and TTL update. Verified that `mContractCodeEntries` already uses the proposed flat `unordered_map<uint256, ...>` pattern, proving the approach works in this codebase. Verified the refactor is localized to `InMemorySorobanState.h/.cpp` plus minor test updates in `InvariantTests.cpp` and `BucketIndexTests.cpp`.

### Code Paths Examined

- `src/ledger/InMemorySorobanState.h:101-277` — `InternalContractDataMapEntry` class hierarchy: `AbstractEntry` virtual base with `ValueEntry` (stores data) and `QueryKey` (for lookups). `ValueEntry::copyKey()` (line 148-153) calls `getTTLKey()` → SHA-256 on EVERY invocation. `ValueEntry::hash()` (line 155-158) delegates to `copyKey()`. `AbstractEntry::operator==` (line 127-131) calls `copyKey()` on both operands — triggers SHA-256 on ValueEntry side per equality check.
- `src/ledger/InMemorySorobanState.h:242-258` — `InternalContractDataMapEntry(LedgerKey)` constructor: creates `make_unique<QueryKey>` (heap alloc), computes `getTTLKey(ledgerKey)` → SHA-256 for CONTRACT_DATA keys.
- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey(LedgerKey)`: confirmed `sha256(xdr::xdr_to_opaque(e))` — XDR serialization + SHA-256 per call.
- `src/ledger/InMemorySorobanState.cpp:52-63` — `updateContractDataTTL()`: erase(iterator) + emplace(new ValueEntry). Emplace triggers `ValueEntry::hash()` → SHA-256. Erase does NOT trigger hash.
- `src/ledger/InMemorySorobanState.cpp:66-89` — `updateTTL()`: constructs `InternalContractDataMapEntry(lk)` (QueryKey, heap alloc + SHA-256), calls `find()` which triggers ValueEntry equality SHA-256 on match, then calls `updateContractDataTTL()` (1 more SHA-256 from emplace). Total: 3 SHA-256 + 1 heap alloc/dealloc per CONTRACT_DATA TTL update.
- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData()`: find(QueryKey) triggers 1 equality SHA-256 + emplace(new ValueEntry) triggers 1 hash SHA-256. Total: 2 SHA-256 + 1 heap alloc/dealloc per update.
- `src/ledger/InMemorySorobanState.cpp:204-218` — `get()` for CONTRACT_DATA: find(QueryKey) triggers 1 equality SHA-256 on match + 1 heap alloc/dealloc.
- `src/transactions/ParallelApplyUtils.cpp:324-386` — `preParallelApplyAndCollectModifiedClassicEntries()`: explicitly SKIPS Soroban entries (line 337-340: `if (isSorobanEntry(lk)) continue;`). So Soroban entries are NEVER in the global entry map.
- `src/transactions/ParallelApplyUtils.cpp:562-607` — `collectClusterFootprintEntriesFromGlobal()`: tries to fetch keys from global map. Soroban keys won't be found there. Thread map starts empty for Soroban keys.
- `src/transactions/ParallelApplyUtils.cpp:700-735` — `getLiveEntryOpt()`: for keys not in `mThreadEntryMap`, falls through to `mInMemorySorobanState.get(key)` for Soroban types (line 725-728). Confirmed every Soroban CONTRACT_DATA key lookup during parallel apply hits InMemorySorobanState.
- `src/ledger/InMemorySorobanState.h:315-317` — `mContractCodeEntries`: declared as `std::unordered_map<uint256, ContractCodeMapEntryT>`, confirming the flat map pattern is already used for ContractCode. The proposed change for ContractData follows the exact same pattern.
- `src/invariant/test/InvariantTests.cpp:625-745` — Test code directly accesses `mContractDataEntries` via `BUILD_TESTS` public visibility. Uses `erase`, `emplace(InternalContractDataMapEntry(...))`, and `begin()`. Would need updating but is straightforward.
- `src/bucket/test/BucketIndexTests.cpp:1134` — Only calls `.size()` on `mContractDataEntries`. Trivial update.

### Findings

**The inefficiency is real and multi-faceted.** The polymorphic `unordered_set` design imposes three independent overhead categories:

1. **Heap allocation per lookup (~100ns × N lookups)**: Every `find()` constructs `InternalContractDataMapEntry(ledgerKey)` → `make_unique<QueryKey>` (heap alloc), then destructs it after the lookup (heap dealloc). A flat map requires only computing the uint256 key and calling `map.find(hash)`.

2. **SHA-256 on stored entries during equality checks (~500ns × N matches)**: When `find()` hits a matching bucket, `AbstractEntry::operator==` calls `ValueEntry::copyKey()` which recomputes `getTTLKey()` → SHA-256 on the STORED entry. With a flat map, equality is a direct uint256 comparison (~2ns). This is the same savings as the reviewed 001-cache-keyhash hypothesis.

3. **Erase/reinsert pattern for mutations (~600-1100ns × N mutations)**: `updateContractData()` and `updateContractDataTTL()` must erase and reinsert because set elements are immutable. Each erase/reinsert cycle: (a) destructs the old ValueEntry (heap dealloc), (b) constructs a new ValueEntry, (c) computes SHA-256 via `hash()` for the emplace. With a flat map, mutations are in-place: `it->second = newValue`.

**The fix is correct and follows existing patterns.** `mContractCodeEntries` (line 323) already uses `unordered_map<uint256, ContractCodeMapEntryT>` — the exact pattern proposed for ContractData. The key insight enabling this is that the uint256 TTL key hash is only 32 bytes, so using it as a map key adds negligible memory overhead compared to the LedgerEntry sizes (typically hundreds to thousands of bytes). The original comment (line 86-87) about "Soroban keys can be quite large... storing them twice would be wasteful" applies to storing the full LedgerKey as a map key, NOT to storing a 32-byte hash.

**Severity assessment:** Conservative estimate of savings from current baseline (no keyhash cache):
- Parallel path: ~600ns savings per get() × 16K-32K lookups per cluster set across 8 threads ≈ ~1-3ms wall clock
- Serial path: ~1-3 SHA-256 + heap overhead savings per mutation × 21K-42K mutations ≈ ~15-25ms
- Total wall clock: ~16-28ms out of ~150-250ms close time ≈ ~6-18%

Rating **Low** conservatively because: (a) the workload estimates (21K entries, 32K+ lookups) are from the hypothesis and not independently verified via profiling, (b) the SHA-256 timing (~500ns) is approximate, and (c) other bottlenecks may dominate at T=8. The true impact could reach Medium but requires benchmark confirmation.

**This subsumes the reviewed 001-cache-keyhash hypothesis entirely.** If the flat map is implemented, the keyhash cache becomes unnecessary — there are no `ValueEntry::hash()` or `ValueEntry::copyKey()` calls to optimize. The flat map also addresses overhead categories (heap alloc, erase/reinsert, virtual dispatch) that keyhash caching does not.

### PoC Guidance

- **Target code**:
  - `src/ledger/InMemorySorobanState.h`: Remove the entire `InternalContractDataMapEntry` class (lines 101-286), including `AbstractEntry`, `ValueEntry`, `QueryKey`, and `InternalContractDataEntryHash`. Replace `mContractDataEntries` (line 315-317) with `std::unordered_map<uint256, ContractDataMapEntryT> mContractDataEntries`. The `ContractDataMapEntryT` struct (lines 48-58) already exists and stores `shared_ptr<LedgerEntry const>` + `TTLData`. Make `ContractDataMapEntryT` fields non-const to enable in-place mutation.
  - `src/ledger/InMemorySorobanState.cpp`: Update all methods that operate on `mContractDataEntries`:
    - `updateContractDataTTL()`: Change from erase/reinsert to `dataIt->second.ttlData = newTtlData` (in-place).
    - `updateContractData()`: Change from erase/reinsert to `dataIt->second = ContractDataMapEntryT(...)` (in-place assign).
    - `createContractDataEntry()`: Compute `uint256 keyHash = getTTLKey(LedgerEntryKey(ledgerEntry)).ttl().keyHash`, then `mContractDataEntries.emplace(keyHash, ContractDataMapEntryT(...))`.
    - `get()` for CONTRACT_DATA: Compute `uint256 keyHash = getTTLKey(ledgerKey).ttl().keyHash`, then `mContractDataEntries.find(keyHash)`.
    - `deleteContractData()`: Same pattern — compute keyHash, find, erase.
    - `updateTTL()`, `createTTL()`, `hasTTL()`, `getTTL()`: For CONTRACT_DATA lookups, compute keyHash from TTL key directly (`ledgerKey.ttl().keyHash`) and do `mContractDataEntries.find(keyHash)`.
    - `initializeStateFromSnapshot()` and `updateState()`: No structural changes, just updated method calls.
    - Copy constructor: Replace `emplace(entry)` loop with iteration over map entries: `mContractDataEntries.emplace(key, ContractDataMapEntryT(make_shared<LedgerEntry const>(*entry.ledgerEntry), entry.ttlData))`.
  - `src/invariant/test/InvariantTests.cpp`: Update direct accesses to `mContractDataEntries` — change from `InternalContractDataMapEntry` construction to flat map operations with uint256 keys.
  - `src/bucket/test/BucketIndexTests.cpp`: Only calls `.size()` — no change needed.
- **Change description**: Replace the polymorphic `unordered_set<InternalContractDataMapEntry>` with a flat `unordered_map<uint256, ContractDataMapEntryT>`, matching the pattern already used by `mContractCodeEntries`. This eliminates heap allocation per lookup, virtual dispatch, SHA-256 recomputation on stored entries, and erase/reinsert overhead for mutations.
- **Correctness check**: Run the full test suite with `[ledger]` and `[soroban]` tags. Key tests: `InvariantTests` (which directly manipulate `mContractDataEntries`), `BucketIndexTests` (which check `mContractDataEntries.size()`), and any Soroban-related integration tests that exercise `InMemorySorobanState::get()` and `updateState()`.
- **Benchmark focus**: Run SAC apply-load benchmark at T=8. Measure median ledger close time. Expected improvement: ~6-18% (Low to Medium range). Also profile `InMemorySorobanState::get()` and `updateState()` specifically to isolate the data structure overhead.
