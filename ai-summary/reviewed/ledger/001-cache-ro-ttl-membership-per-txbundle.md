# H001: Cache Read-Only TTL Membership Per `TxBundle`

**Date**: 2026-04-10
**Subsystem**: ledger, transactions
**Severity**: Low
**Impact**: worker-thread CPU and allocator overhead in T=8 Soroban apply
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The parallel-apply commit path should determine whether a modified key is a
read-only TTL bump from data that was already computed when the stage was built.
For a given Soroban transaction, the read-only footprint is immutable, so the
worker should not allocate and rebuild a fresh TTL-membership hash set after the
host has already finished executing the transaction.

## Mechanism

`ThreadParallelApplyLedgerState::commitChangesFromSuccessfulTx` rebuilds an
`UnorderedSet<LedgerKey>` by calling `buildRoTTLSet(txBundle, mTTLKeyCache)` for
every successful Soroban transaction, even though `ApplyStage` has already
precomputed the stage-wide TTL key cache and the transaction footprint never
changes. This adds an allocation-heavy read-only-footprint scan to the worker
hot path after every host invocation, then immediately discards the set after
one pass over `res.getModifiedEntryMap()`.

Persisting per-transaction read-only TTL membership in `TxBundle` (or another
stage-owned structure) would turn this into reuse of already-known data instead
of thousands of small hash-table builds. The savings should be most visible in
the T=8 apply-load scenarios where many workers do this bookkeeping in
parallel.

## Trigger

Run `scripts/run_apply_load_matrix.py` for any Soroban scenario, especially
`sac` at `T=8`. Profile worker time after the host returns and measure time and
allocations in `buildRoTTLSet` plus the subsequent `roTTLSet.find(key)` lookups.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:buildRoTTLSet:127-143` — rebuilds RO TTL membership from the tx footprint on every success
- `src/transactions/ParallelApplyUtils.cpp:commitChangesFromSuccessfulTx:819-829` — calls `buildRoTTLSet` for each successful tx
- `src/transactions/ParallelApplyUtils.cpp:commitChangeFromSuccessfulTx:748-773` — probes the freshly-built set for every modified key
- `src/transactions/ParallelApplyStage.cpp:precomputeKeyCaches:19-50` — already precomputes related TTL-key data once per stage
- `src/transactions/ParallelApplyStage.h:TxBundle:64-104` — natural place to store precomputed per-tx RO TTL membership

## Evidence

- `ApplyStage::precomputeKeyCaches` already walks every Soroban footprint and
  caches `LedgerKey -> TTL key`, so the codebase already accepts extra stage
  memory to remove repeated TTL-key derivation.
- `buildRoTTLSet` performs a new `UnorderedSet` allocation and `emplace` loop
  over `footprint.readOnly` even though `txBundle` is immutable at this point.
- The call happens in the worker post-host path, not once per stage, so the
  cost scales with successful transaction count rather than stage count.
- The benchmark workloads run thousands of successful Soroban transactions per
  ledger, multiplying even modest per-tx allocator work.

## Anti-Evidence

- Some apply-load transactions may have small read-only footprints, which would
  cap the savings.
- Storing another per-transaction container increases memory footprint for
  `ApplyStage`, so a compact representation may be required.
- If profiling shows host execution still dwarfs this bookkeeping cost, the
  improvement may stay near the low end of the severity scale.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced `commitChangesFromSuccessfulTx` (line 818) which is called per successful Soroban tx in `LedgerManagerImpl::applyThread` (line 2406). It constructs a fresh `UnorderedSet<LedgerKey>` via `buildRoTTLSet` (lines 127-143) by iterating the immutable RO footprint, looking up each soroban key in the stage-wide `mTTLKeyCache`, and emplacing the TTL keys. The set is then probed in `commitChangeFromSuccessfulTx` (line 758) for every entry in the tx's modified entry map, then immediately destroyed. The RO footprint and TTL key cache are both immutable at this point, confirming the set could be precomputed once per TxBundle.

### Code Paths Examined

- `src/transactions/ParallelApplyUtils.cpp:buildRoTTLSet:127-143` — Allocates `UnorderedSet<LedgerKey>`, iterates `footprint.readOnly`, filters on `isSorobanEntry`, looks up TTL keys in `mTTLKeyCache`, emplaces into set. Confirmed entirely deterministic from immutable inputs.
- `src/transactions/ParallelApplyUtils.cpp:commitChangesFromSuccessfulTx:818-830` — Calls `buildRoTTLSet` then passes the set to per-key `commitChangeFromSuccessfulTx`. Set is stack-local and destroyed at scope exit.
- `src/transactions/ParallelApplyUtils.cpp:commitChangeFromSuccessfulTx:747-774` — Uses `roTTLSet.find(key)` (line 758) to decide whether a modified entry is an RO TTL bump that should be accumulated rather than written to the entry map.
- `src/ledger/LedgerManagerImpl.cpp:applyThread:2380-2417` — Confirms `commitChangesFromSuccessfulTx` is called once per successful tx in the worker loop.
- `src/transactions/ParallelApplyStage.cpp:precomputeKeyCaches:18-51` — Already iterates all TxBundles and populates `mTTLKeyCache` and `mReadWriteKeys`. Confirmed this is the natural place to also build per-TxBundle RO TTL sets.
- `src/transactions/ParallelApplyStage.h:TxBundle:64-104` — Simple data class. Adding a precomputed set is structurally feasible.
- `src/ledger/LedgerHashUtils.h:136-202` — TTL key hash uses `std::hash<uint256>()(lk.ttl().keyHash)` — cheap. CONTRACT_DATA key hash uses `shortHash::xdrComputeHash` — more expensive and currently paid per-tx in the cache lookup.

### Findings

The inefficiency is **real and confirmed**: `buildRoTTLSet` performs a fresh heap allocation, 2–5 `mTTLKeyCache` lookups (involving CONTRACT_DATA/CONTRACT_CODE hashing), 2–5 emplace operations, and destruction per successful Soroban tx. The fix is correct — the RO footprint is immutable, and precomputation in `precomputeKeyCaches` eliminates all per-tx allocation and hashing.

**However, the per-call cost is small.** Typical Soroban RO footprints contain 2–5 entries. Each `buildRoTTLSet` call involves a small hash set (likely 5 buckets), a few cache lookups, and a few emplaces. At 3200 txs/ledger (SAC T=8 benchmark), the total overhead is estimated at 1–5ms per ledger — well under 5% of typical ledger close time.

**Severity downgraded to Informational.** The optimization is correct and worth doing for code cleanliness and to remove unnecessary allocator pressure on worker threads, but it is unlikely to produce a measurable improvement (≥5%) in any benchmark scenario.

### PoC Guidance

- **Target code**: `src/transactions/ParallelApplyStage.h` (add `UnorderedSet<LedgerKey>` member to `TxBundle` or a per-TxBundle map to `ApplyStage`), `src/transactions/ParallelApplyStage.cpp` (populate in `precomputeKeyCaches`), `src/transactions/ParallelApplyUtils.cpp` (remove `buildRoTTLSet` calls, accept precomputed set from TxBundle)
- **Change description**: In `precomputeKeyCaches()`, build the RO TTL set for each TxBundle and store it. In `commitChangesFromSuccessfulTx`, use the stored set instead of calling `buildRoTTLSet`. Note: `precomputeKeyCaches` currently iterates by const ref; either make it non-const or store the sets in a separate container in `ApplyStage` indexed by TxBundle position.
- **Correctness check**: Existing parallel apply tests (`[tx]` and `[soroban]` tags) cover this path. The precomputed set must be identical to what `buildRoTTLSet` would produce — assert equality in debug builds.
- **Benchmark focus**: SAC T=8 scenario. Expect <1% improvement on median close time. Primary benefit is reduced allocator contention across worker threads.
