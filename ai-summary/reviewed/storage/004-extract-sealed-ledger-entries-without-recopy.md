# H009: Replace post-seal entry copy amplification with a single extraction pass

**Date**: 2026-04-10
**Subsystem**: storage (ledger, bucket)
**Severity**: Low
**Impact**: post-apply commit CPU and memory-bandwidth reduction
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

After `LedgerTxn` is sealed, the modified-entry set should be materialized once
and then shared across downstream consumers. The commit path should not deep-copy
the same `LedgerEntry` payloads into transient init/live vectors, then deep-copy
them again into `BucketEntry` objects, and then deep-copy Soroban entries again
into the in-memory state cache.

## Mechanism

`finalizeLedgerTxnChanges` calls `ltx.getAllEntries(...)`, which walks the
sealed `mEntry` map and copies every modified ledger entry into
`std::vector<LedgerEntry>` / `std::vector<LedgerKey>`. `addLiveBatch(...)`
immediately re-copies those vectors into `BucketEntry` objects in
`LiveBucket::convertToBucketEntry(...)`, and `updateInMemorySorobanState(...)`
then copies Soroban entries again into heap-owned `shared_ptr<LedgerEntry const>`
records. On Soroban-heavy ledgers, this creates multiple full-object copies of
the same changed entries on the single-threaded commit path after the parallel
execution phase has already finished.

## Trigger

Any apply-load Soroban ledger that modifies thousands of entries. The hot path
is reached on every ledger close after tx application when the code seals the
ledger transaction, emits bucket inputs, and refreshes `InMemorySorobanState`.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:3039-3046` — `getAllEntries(...)` feeds both `addLiveBatch(...)` and `updateInMemorySorobanState(...)`
- `src/ledger/LedgerTxn.cpp:1627-1667` — first full copy out of `mEntry` into init/live/dead vectors
- `src/bucket/LiveBucket.cpp:379-419` — second copy into `BucketEntry` before sort/write
- `src/ledger/InMemorySorobanState.h:224-235` — stored contract-data values are heap-copied into new `shared_ptr`s
- `src/ledger/InMemorySorobanState.cpp:109-110` — updates reinsert a copied `LedgerEntry`
- `src/ledger/InMemorySorobanState.cpp:140-141` — creates insert a copied `LedgerEntry`
- `src/ledger/InMemorySorobanState.cpp:299-301` — contract-code updates also replace with copied `LedgerEntry`

## Evidence

The current code performs at least one full pass to copy every changed ledger
entry out of `LedgerTxn`, then immediately performs more full passes over the
same data for bucket ingestion and in-memory-state refresh. The bucket path only
needs stable post-seal data and the in-memory state only needs durable ownership,
so a dedicated extraction API could build owned `BucketEntry` objects and
long-lived Soroban cache payloads from one traversal of `mEntry` instead of
serially re-copying the same XDR objects.

## Anti-Evidence

Some copying is unavoidable: bucket writes need owned sorted entries, and the
in-memory cache persists across ledgers. The refactor is intrusive because it
changes ownership and lifetime assumptions around a sealed `LedgerTxn`, and it
must preserve the later SQL commit path. The actual benchmark gain depends on
how large modified entries are in the apply-load workloads.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full post-seal commit path in `finalizeLedgerTxnChanges`
(LedgerManagerImpl.cpp:3049-3056). Confirmed that `getAllEntries` copies every
modified entry from the sealed `mEntry` hash map into intermediate
`vector<LedgerEntry>` vectors (copy 1). Then `convertToBucketEntry`
(LiveBucket.cpp:379-419) copies each `LedgerEntry` into a local `BucketEntry`
AND copies that local into the output vector via `push_back(ce)` without
`std::move` (copies 2a + 2b). Finally `updateState` in InMemorySorobanState
copies Soroban entries into `make_shared<LedgerEntry const>` (copy 3). Total:
3 copies for non-Soroban entries, 4 copies for Soroban entries. The most
impactful finding is the gratuitous double-copy in `convertToBucketEntry` where
`push_back(ce)` copies a local `BucketEntry` that could trivially be moved.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:3049-3056` — sequential call to `getAllEntries`, `addAnyContractsToModuleCache`, `addLiveBatch`, `updateInMemorySorobanState`; all take `const&` to the same vectors
- `src/ledger/LedgerTxn.cpp:1637-1667` — `getAllEntries` iterates sealed `mEntry` map, copies each `entry->ledgerEntry()` (which returns `LedgerEntry const&`) via `emplace_back` into output vectors
- `src/ledger/LedgerTxn.cpp:2333-2351` — `maybeUpdateLastModifiedThenInvokeThenSeal` invokes callback with `mEntry` then marks sealed; `mEntry` persists after seal
- `src/bucket/LiveBucket.cpp:390-410` — `convertToBucketEntry` constructs local `BucketEntry ce`, copies `LedgerEntry` into `ce.liveEntry()`, then copies `ce` into vector via `bucket.push_back(ce)` (lvalue → copy overload selected)
- `src/bucket/LiveBucket.cpp:467-498` — `freshInMemoryOnly` calls `convertToBucketEntry` then `std::move(entries)` into the bucket; the move is already present at this level but the copies inside `convertToBucketEntry` are not optimized
- `src/bucket/BucketListBase.cpp:196-223` — `prepareFirstLevel` for level 0: synchronous in-memory path, no background thread
- `src/ledger/InMemorySorobanState.cpp:536-602` — `updateState` iterates all entries, calls per-type create/update/delete; each constructs `make_shared<LedgerEntry const>(ledgerEntry)` which deep-copies
- `src/ledger/InMemorySorobanState.h:224-228` — `InternalContractDataMapEntry(LedgerEntry const&, TTLData)` constructor: `make_shared<LedgerEntry const>(ledgerEntry)` — explicit deep copy
- `src/ledger/InMemorySorobanState.cpp:299-301` — `updateContractCode` also `make_shared<LedgerEntry const>(ledgerEntry)` — deep copy

### Findings

**Copy amplification confirmed.** For SAC 3200 benchmark (~15,000 modified
entries per ledger, average ~175 bytes each):

| Copy | Location | What | Est. cost |
|------|----------|------|-----------|
| 1 | `getAllEntries` | `mEntry` → `vector<LedgerEntry>` (scattered reads from hash map) | ~3ms |
| 2a | `convertToBucketEntry` | `LedgerEntry` → local `BucketEntry` | ~3ms |
| 2b | `convertToBucketEntry` | local `BucketEntry` → `vector<BucketEntry>` via `push_back(ce)` | ~3ms |
| 3 | `updateState` | `LedgerEntry` → `make_shared` (Soroban only, ~10k entries) | ~1.5ms |
| **Total** | | | **~10.5ms** |

The most wasteful is copy 2b: `push_back(ce)` where `ce` is a local that is
never used after the push. Simply adding `std::move` eliminates this entirely.

**Impact estimate for T=8 scenarios:** With parallel apply reducing
application time to ~40ms, the serial commit path (~50-70ms) dominates.
Eliminating copies 2a+2b saves ~6ms, which is 5-9% of the ~70-100ms total
ledger close at T=8. This reaches Low severity.

**The full "single extraction pass" is architecturally invasive.** It would
require coupling LedgerTxn's sealed-entry iteration directly to Bucket and
InMemorySorobanState construction, breaking clean subsystem boundaries. The
simpler move-semantics approach captures most of the benefit with minimal
code changes.

### PoC Guidance

**Phase 1 — Trivial fix (no interface changes):**
- **Target code**: `src/bucket/LiveBucket.cpp:convertToBucketEntry` (lines 390-410)
- **Change**: Replace `bucket.push_back(ce)` with `bucket.push_back(std::move(ce))` in all three loops (initEntries, liveEntries, deadEntries). Alternatively, construct directly in the vector using `emplace_back()` + modify `back()`.
- **Correctness check**: All existing bucket tests (`[bucket]` tag), plus `[bucketlist]` and `[mergealiases]`. The sorted order and dedup check on lines 412-418 will validate correctness.
- **Benchmark focus**: `bucket.add_live_batch` timer; expect ~3ms reduction at SAC 3200.

**Phase 2 — Move semantics through addLiveBatch:**
- **Target code**: `src/ledger/LedgerManagerImpl.cpp:3049-3056` — reorder so `updateInMemorySorobanState` is called before `addLiveBatch`, then pass `std::move(initEntries), std::move(liveEntries), std::move(deadEntries)` to `addLiveBatch`
- **Target code**: `src/bucket/LiveBucket.cpp:convertToBucketEntry` — change signature to take `std::vector<LedgerEntry>&&` and use `std::move(e)` in the copy loop
- **Target code**: `src/bucket/BucketManager.cpp:addLiveBatch`, `src/bucket/LiveBucketList.cpp:addBatch`, `src/bucket/BucketListBase.cpp:addBatchInternal`, `src/bucket/BucketListBase.cpp:prepareFirstLevel` — propagate rvalue-ref / by-value signatures through the chain
- **Correctness check**: Same bucket tests plus `[ledger]` tag tests. Verify `updateInMemorySorobanState` sees unmodified entries before the move.
- **Benchmark focus**: End-to-end ledger close; expect ~6ms total reduction, most visible in T=8 scenarios (SAC 3200/T=8).
