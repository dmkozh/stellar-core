# H023: Cache TTL Key Hashes Across the Apply Pipeline to Avoid Recomputation

**Date**: 2025-07-14
**Subsystem**: ledger
**Severity**: Medium
**Impact**: CPU — avoids redundant SHA-256 computations during updateInMemorySorobanState
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When a CONTRACT_DATA entry's TTL key hash (`getTTLKey(lk).ttl().keyHash`) is
computed during parallel apply for `InMemorySorobanState::get()` lookups, the
computed hash should be preserved and reused when the same entry is later
processed in `updateInMemorySorobanState()`. Each hash computation involves
`xdr::xdr_to_opaque()` (heap alloc + XDR serialize) followed by `sha256()`
(~500ns total). Avoiding recomputation for ~21K entries would save ~10ms.

## Mechanism

The TTL key hash for CONTRACT_DATA entries is computed at multiple points in
the apply pipeline:

1. **During parallel apply** (`InMemorySorobanState::get()` via
   `ThreadParallelApplyLedgerState::getLiveEntryOpt`): Computes hash to look
   up the entry. Result is discarded after the lookup.

2. **During `updateInMemorySorobanState()`** (called from
   `finalizeLedgerTxnChanges` line 3045): For each CONTRACT_DATA entry in
   `initEntries` and `liveEntries`, calls `updateContractData()` or
   `createContractDataEntry()`, which construct an
   `InternalContractDataMapEntry` that recomputes the hash.

The entry data flows through `commitChangesToLedgerTxn` →
`getAllEntries` → `updateState` as bare `LedgerEntry` objects with no
associated hash metadata. The hash is recomputed from scratch each time.

## Trigger

Run SAC apply-load benchmark at T=8 with 3200 transactions per ledger.
Each ledger modifies ~21K unique CONTRACT_DATA entries, each requiring
hash recomputation in `updateState`.

## Target Code

- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData()` calls `find()` with hash recomputation
- `src/ledger/InMemorySorobanState.cpp:114-142` — `createContractDataEntry()` calls `find()` + `getTTLKey()`
- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey()`: `sha256(xdr::xdr_to_opaque(e))`
- `src/transactions/ParallelApplyUtils.cpp:723-728` — `InMemorySorobanState::get()` during parallel apply

## Evidence

1. `getTTLKey()` calls `xdr::xdr_to_opaque(e)` (heap-allocates a temporary
   `vector<uint8_t>`) followed by `sha256()`. Each call costs ~500–600ns.
2. For 21K CONTRACT_DATA entries modified per SAC ledger, recomputation in
   `updateState` alone costs ~10–12ms.
3. The hash was already computed during parallel apply lookups but is not
   preserved through the `LedgerTxn` → `getAllEntries` pipeline.

## Anti-Evidence

1. The `getAllEntries` interface returns `vector<LedgerEntry>`, which has no
   slot for metadata like a precomputed hash. Adding hash metadata would
   require changing this interface (and `addLiveBatch`, `updateState`, etc.).
2. Not all entries in `updateState` had a prior `get()` lookup — newly created
   entries (initEntries) may not have been looked up during parallel apply.
3. Thread-safety: the parallel apply lookups happen on worker threads while
   `updateState` runs on the main thread. A shared cache would need
   synchronization.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-14
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The implementation is too invasive for the expected benefit:

1. **Interface changes required**: The entry pipeline (`LedgerTxn` →
   `getAllEntries` → `addLiveBatch` / `updateState`) passes entries as bare
   `LedgerEntry` and `LedgerKey` objects throughout. Adding a precomputed
   hash would require either: (a) changing these interfaces to pass
   `(LedgerEntry, optional<uint256>)` pairs through every function in the
   chain, or (b) maintaining a separate thread-safe lookup table. Both
   approaches are invasive and error-prone.

2. **H001 (flat map) subsumes this**: If H001 (hypothesis 001 in this batch)
   is implemented, the flat `unordered_map<uint256, ContractDataValue>` would
   make the per-operation hash lookup a single map access. While the SHA-256
   key derivation cost remains, the flat map eliminates all the additional
   overhead (heap alloc, virtual dispatch, erase/reinsert) that makes the
   current design expensive. The total remaining SHA-256 cost (~10ms) is at
   the margin of Low severity, and caching it through the pipeline adds
   disproportionate complexity.

3. **Partial coverage**: Not all entries processed by `updateState` were
   previously looked up during parallel apply. Newly created entries
   (initEntries) and entries from failed transactions would not benefit from
   a cache, reducing the effective savings below the estimated ~10ms.

### Lesson Learned

When the same computation is repeated across different phases of a pipeline,
the correct fix is usually to redesign the data structure to avoid the
computation entirely (as in the flat map approach), rather than threading
cached results through a complex pipeline. The flat map in H001 eliminates
the polymorphic overhead that makes the hash recomputation expensive.
