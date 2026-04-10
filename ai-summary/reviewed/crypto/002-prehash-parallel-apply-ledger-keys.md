# H002: Prehash Parallel-Apply `LedgerKey`s Instead Of Re-XDR-Hashing On Every Map Lookup

**Date**: 2026-04-10
**Subsystem**: crypto
**Severity**: High
**Impact**: parallel apply CPU and scalability lost to repeated `LedgerKey` hashing
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Parallel apply should compute the hash of a hot `LedgerKey` once when the key is
constructed for stage/thread/tx bookkeeping and then reuse that hash across all
subsequent map/set lookups and copies. Contract-data-heavy workloads should not
re-run XDR hashing of the same `SCVal` key every time they touch
`mGlobalEntryMap`, `mThreadEntryMap`, `mTxEntryMap`, `mRoTTLBumps`, or the
stage-level read/write sets.

## Mechanism

The hot parallel-apply containers are all typed as `UnorderedMap<LedgerKey, ...>`
or `UnorderedSet<LedgerKey>`, so every `find`, `emplace`, and
`insert_or_assign` re-enters `std::hash<LedgerKey>`. For `CONTRACT_DATA` this
hash path walks through `std::hash<SCAddress>` and
`shortHash::xdrComputeHash(lk.contractData().key)`, reserializing the `SCVal`
key on every lookup. The codebase already has an exact prior-art wrapper for
this problem: `InternalLedgerKey` lazily caches `mHash`, and
`InMemorySorobanState` caches contract key hashes because repeated key hashing
was expensive enough to warrant it. Converting the parallel-apply maps/sets to
an immutable prehashed key wrapper would pay that cost once per key instead of
once per lookup.

## Trigger

Run `custom_token` or `soroswap` apply-load at `T=8` and sample
`std::hash<LedgerKey>`, `shortHash::xdrComputeHash`, and the hot map operations
in `ParallelApplyUtils`. Compare against a build that stores prehashed
`InternalLedgerKey`-style wrappers in the stage/thread/tx maps and sets.

## Target Code

- `src/transactions/TransactionFrameBase.h:52-53` — `TxModifiedEntryMap = UnorderedMap<LedgerKey, ...>`
- `src/transactions/TransactionFrameBase.h:92-99` — all parallel-apply entry maps are `UnorderedMap<LedgerKey, ...>`
- `src/transactions/ParallelApplyUtils.cpp:99-117` — stage read/write key set inserts raw `LedgerKey`s
- `src/transactions/ParallelApplyUtils.cpp:148-170` — RO TTL bookkeeping hashes raw `LedgerKey`s
- `src/transactions/ParallelApplyUtils.cpp:353-355` — global map population stores raw `LedgerKey`s
- `src/transactions/ParallelApplyUtils.cpp:578-586` — cluster preload repeatedly `find()`s and `emplace()`s raw keys
- `src/transactions/ParallelApplyUtils.cpp:640-657` — RO TTL bump flushing repeatedly probes raw-key maps
- `src/transactions/ParallelApplyUtils.cpp:702-749` — thread-state reads and updates hash raw keys again
- `src/transactions/ParallelApplyUtils.cpp:771-840` — successful tx commit path repeatedly probes and inserts raw keys
- `src/transactions/ParallelApplyUtils.cpp:894-966` — tx-local reads/writes hash raw keys in `mTxEntryMap`
- `src/ledger/LedgerHashUtils.h:136-203` — `std::hash<LedgerKey>` recomputes the full hash from the XDR key each lookup
- `src/ledger/InternalLedgerEntry.cpp:191-221` — `InternalLedgerKey::hash()` already caches a computed hash for repeated lookups
- `src/ledger/InMemorySorobanState.h:140-172` — in-memory Soroban state explicitly caches contract key hashes to avoid repeated hashing work

## Evidence

The parallel-apply pipeline uses the same logical key in multiple container
layers — stage set, global map, thread map, tx map, and restoration/TTL side
maps — but none of those layers carry a cached hash. The codebase already
contains two independent signals that this class of work matters: a dedicated
`InternalLedgerKey` hash cache and an `InMemorySorobanState` comment explaining
that repeated key hashing was expensive enough to precompute once.

## Anti-Evidence

This requires a careful immutable-key wrapper so cached hashes cannot become
stale, and conversions at container boundaries add some one-time cost of their
own. The hypothesis is strongest for `CONTRACT_DATA`/TTL-heavy workloads where
the same complex keys are probed many times; simpler classic-key workloads may
see less benefit.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full lifecycle of a `CONTRACT_DATA` LedgerKey through the parallel apply pipeline. A single key is hashed ~15-25 times across stage sets (`getReadWriteKeysForStage`), global map population, cluster preload (`collectClusterFootprintEntriesFromGlobal`), tx-level lookups (`getLiveEntryOpt` chains through `mTxEntryMap` → `mThreadEntryMap`), upserts, RO TTL bookkeeping, and the commit path (`commitChangesFromSuccessfulTx`). For `CONTRACT_DATA` keys, each hash invokes `shortHash::xdrComputeHash(lk.contractData().key)`, which constructs an `XDRShortHasher` (acquiring `gKeyMutex`), XDR-serializes the `SCVal` key into a SipHash-2,4 computation, and finalizes. The prior-art `InternalLedgerKey::hash()` caches `mHash` on first call and returns instantly on subsequent calls — this pattern is absent from all parallel apply containers.

### Code Paths Examined

- `src/ledger/LedgerHashUtils.h:136-203` — `std::hash<LedgerKey>` dispatches by type. CONTRACT_DATA (line 178-184) calls `std::hash<SCAddress>` (cheap for contract addresses: just `std::hash<uint256>` on `contractId()`) then `shortHash::xdrComputeHash(lk.contractData().key)` (expensive: XDR serialization + SipHash of full SCVal). TTL keys (line 194-196) only hash a `uint256` — very cheap.
- `src/crypto/ShortHash.cpp:74-79` — `XDRShortHasher` constructor acquires `gKeyMutex` to copy 16-byte key, then actual SipHash runs lock-free. `computeHash` (line 62-72) holds the mutex for the entire hash.
- `src/crypto/ShortHash.h:47-55` — `xdrComputeHash<T>` creates fresh `XDRShortHasher`, archives XDR, flushes, and returns digest. No caching.
- `src/transactions/ParallelApplyUtils.cpp:99-117` — `getReadWriteKeysForStage`: each key hashed on `emplace` into `unordered_set<LedgerKey>` plus TTL key emplace
- `src/transactions/ParallelApplyUtils.cpp:577-607` — `collectClusterFootprintEntriesFromGlobal`: per-key: `mThreadEntryMap.find(key)` + `globalEntryMap.find(key)` + `mThreadEntryMap.emplace(key,...)` = 3 hashes, doubled with TTL key = 6 hashes per Soroban footprint entry
- `src/transactions/ParallelApplyUtils.cpp:700-749` — `getLiveEntryOpt`: `mThreadEntryMap.find(key)` = 1 hash; `upsertEntry`: `mThreadEntryMap.insert_or_assign(key,...)` = 1 hash
- `src/transactions/ParallelApplyUtils.cpp:890-950` — Tx-level `getLiveEntryOpt`: `mTxEntryMap.find(key)` (1 hash) + fallback to `mThreadState.getLiveEntryOpt(key)` (1 hash); `upsertEntry`: `getLiveEntryOpt` (2 hashes) + `mTxEntryMap.insert_or_assign` (1 hash) = 3 hashes per upsert
- `src/transactions/ParallelApplyUtils.cpp:831-843` — `commitChangesFromSuccessfulTx`: iterates modified entries, calling `commitChangeFromSuccessfulTx` which calls `getLiveEntryOpt` (2 hashes) + `roTTLSet.find(key)` (1 hash) + `upsertEntry`/`eraseEntry` (1 hash) = 4 hashes per entry
- `src/ledger/InternalLedgerEntry.cpp:191-221` — `InternalLedgerKey::hash()` caches into `mutable mHash` on first call, returns cached value on subsequent calls. Exactly the pattern the parallel apply maps lack.
- `src/ledger/InternalLedgerEntry.h:39` — `size_t mutable mHash` member enables the lazy hash caching

### Findings

**The inefficiency is real.** A `CONTRACT_DATA` key entering the parallel apply pipeline is hashed approximately 15-25 times across 5 distinct map/set layers (stage set, global map, thread map, tx map, RO TTL map). Each hash of a CONTRACT_DATA key involves: (1) constructing an `XDRShortHasher` which acquires `gKeyMutex` to copy the 16-byte SipHash key, (2) XDR-serializing the `SCVal` key field into the SipHash state via the `XDRHasher` CRTP framework, and (3) finalizing the SipHash-2,4 digest. The XDR traversal is the dominant cost — it walks the full SCVal tree with endian swaps and padding.

**It is in a hot path.** Every Soroban transaction touches these maps multiple times per footprint entry. At T=8 with thousands of transactions per ledger, this totals hundreds of thousands of hash operations per ledger close.

**The proposed fix is correct and has prior art.** `InternalLedgerKey` already demonstrates this pattern works: a `mutable size_t mHash` member that caches on first call. The parallel apply maps could use either: (a) `InternalLedgerKey` directly (requires all key types go through it), or (b) a lighter-weight `HashedLedgerKey` wrapper that stores `LedgerKey` + cached `size_t hash`. No callers modify map keys after insertion, so staleness is not a concern.

**Impact is estimated as Low, not High.** The related hypothesis H006 (removing the `gKeyMutex` from the same hash path) was benchmarked and showed no net improvement — in fact, the most relevant scenario (`custom_token,TX=3000,T=8`) regressed by 12.5%. While H002 eliminates ~5-25x more overhead per operation than H006 (full hash computation vs. just mutex acquisition), the H006 benchmark data is a cautionary signal that hash overhead may not dominate ledger close time. Estimated per-key savings: ~100-400ns per avoided recomputation × ~15-20 avoided recomputations = ~1.5-8μs per CONTRACT_DATA key. With ~1000-5000 CONTRACT_DATA keys per ledger, total savings would be ~1.5-40ms against total close times of ~100-500ms = roughly 1-8% improvement.

**Complication: implementation scope is wide.** Changing the key type of 5+ map/set typedefs across `TransactionFrameBase.h`, `ParallelApplyUtils.h`, and `ParallelApplyUtils.cpp` requires careful surgery. Every location that constructs a key, passes a key by reference, or iterates over entries must be updated. The `LedgerKey` values come from XDR footprints (read-only) and are used as keys in multiple independent containers — wrapping them once at ingestion and propagating the wrapper through the pipeline is feasible but touches ~30-50 call sites.

### PoC Guidance

- **Target code**: 
  - `src/transactions/TransactionFrameBase.h` — change `TxModifiedEntryMap`, `ParallelApplyEntryMap`, and related typedefs from `UnorderedMap<LedgerKey, ...>` to `UnorderedMap<HashedLedgerKey, ...>` (or reuse `InternalLedgerKey`)
  - `src/transactions/ParallelApplyUtils.cpp` — wrap `LedgerKey`s from XDR footprints at ingestion points (`getReadWriteKeysForStage`, `loadFootprintEntries`, `collectClusterFootprintEntriesFromGlobal`) and propagate the wrapper
  - `src/transactions/ParallelApplyUtils.h` — update `mRoTTLBumps`, `mThreadRestoredEntries`, and related containers
- **Change description**: Create a lightweight `HashedLedgerKey` (or reuse `InternalLedgerKey`) that lazily caches the hash on first computation. Replace `LedgerKey` as map key type in all parallel apply containers. Wrap keys once at footprint ingestion and propagate through the pipeline. The simplest approach: add a `mutable size_t mHash = 0` directly to the existing `LedgerKey` hash specialization using a wrapper, following the `InternalLedgerKey` pattern exactly.
- **Correctness check**: Run `[tx]` and `[ledger]` tagged tests — these cover the parallel apply paths extensively. The key correctness invariant: hash values must be identical to the unwrapped `std::hash<LedgerKey>` computation.
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` with T=8 `custom_token` and `soroswap` scenarios. Focus on median and p99 ledger close time. Expected improvement: 5-10% on CONTRACT_DATA-heavy workloads. Given H006's null result on the mutex-only component, this broader optimization may show a clearer signal.
