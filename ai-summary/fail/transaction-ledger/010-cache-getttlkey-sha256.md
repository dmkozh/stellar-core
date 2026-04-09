# H002: Cache getTTLKey SHA256 Computations to Eliminate Redundant Crypto Hashing

**Date**: 2026-04-08
**Subsystem**: transaction-ledger (ledger/LedgerTypeUtils, transactions/ParallelApplyUtils)
**Severity**: Low
**Impact**: 5-10% improvement on T=8 scenarios by eliminating redundant SHA256 in sequential bottlenecks
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`getTTLKey(ledgerKey)` should compute SHA256 of the XDR-serialized key at
most once per unique key per ledger close. Subsequent calls for the same
key should return a cached result.

## Mechanism

`getTTLKey` (LedgerTypeUtils.cpp:31-38) computes
`sha256(xdr::xdr_to_opaque(e))` — an XDR serialization + SHA256 hash —
every time it is called, with no caching. Multiple call sites invoke it
repeatedly for the same keys during a single ledger close:

1. `collectClusterFootprintEntriesFromGlobal` (line 601-603): calls
   `getTTLKey(key)` for every Soroban key in every tx's footprint
2. `getReadWriteKeysForStage` (line 111-113): calls `getTTLKey(lk)` for
   every readWrite Soroban key in every tx — called once per stage
3. `flushRoTTLBumpsInTxWriteFootprint` (line 639): calls `getTTLKey(lk)`
   for every readWrite Soroban key per tx
4. `InMemorySorobanState::get` for CONTRACT_DATA/CONTRACT_CODE: implicitly
   calls `getTTLKey` via `InternalContractDataMapEntry(ledgerKey)` constructor

For a ledger with 200 Soroban txs, 20 Soroban keys per tx footprint,
and 2 stages:
- Step 1: 200 × 20 = 4000 getTTLKey calls (sequential)
- Step 2: 200 × 10 (rw only) = 2000 calls per stage × 2 = 4000 (sequential)
- Step 3: 200 × 10 = 2000 calls (parallel across 8 threads)
- Step 4: varies, ~4000 calls (parallel)

Total: ~14000 getTTLKey calls. At ~700ns each (XDR serialize + SHA256):
~9.8ms per ledger. With only ~4000 unique keys, ~10000 are redundant,
wasting ~7ms.

Critically, steps 1 and 2 run sequentially on the apply thread, directly
extending the serial portion of the ledger apply. Per Amdahl's law, this
serial overhead limits T=8 scalability.

## Trigger

Run the apply-load benchmark with T=8 threads and soroswap or
custom_token transactions. Profile `getTTLKey` call frequency and total
time using Tracy or `perf record`.

## Target Code

- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey`: computes `sha256(xdr::xdr_to_opaque(e))` with no cache
- `src/transactions/ParallelApplyUtils.cpp:100-118` — `getReadWriteKeysForStage`: calls `getTTLKey` for each readWrite Soroban key
- `src/transactions/ParallelApplyUtils.cpp:562-607` — `collectClusterFootprintEntriesFromGlobal`: calls `getTTLKey` for each Soroban key
- `src/transactions/ParallelApplyUtils.cpp:626-659` — `flushRoTTLBumpsInTxWriteFootprint`: calls `getTTLKey` per readWrite key per tx
- `src/ledger/InMemorySorobanState.cpp:211-212` — `get()` for CONTRACT_DATA: constructs `InternalContractDataMapEntry` which calls `getTTLKey`

## Evidence

- `getTTLKey` implementation (LedgerTypeUtils.cpp:36): `k.ttl().keyHash = sha256(xdr::xdr_to_opaque(e));` — no caching whatsoever
- The same LedgerKeys appear in multiple call sites during a single ledger close (footprint keys are iterated in collectClusterFootprints, getReadWriteKeysForStage, and flushRoTTLBumps)
- SHA256 is a cryptographic hash function with ~500ns cost for small inputs
- `xdr::xdr_to_opaque(e)` allocates a vector and serializes, adding ~200ns
- CONTRACT_DATA keys can have large SCVal components (100-500 bytes), increasing serialization cost

## Anti-Evidence

- For CONTRACT_CODE keys, InMemorySorobanState uses the keyHash directly from TTL keys (no SHA256 needed for TTL key lookups)
- Some call sites work with TTL keys directly (already have the hash), bypassing getTTLKey
- The cost per call (~700ns) is small relative to VM execution time per tx (~2-5ms)
- Adding a cache introduces memory overhead and cache invalidation complexity
- A simple `unordered_map<LedgerKey, LedgerKey>` cache per stage would add ~40KB for 4000 entries but eliminate ~7ms of redundant hashing

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced `getTTLKey` (LedgerTypeUtils.cpp:31-38) through all call sites in the parallel apply path. Confirmed the function unconditionally performs XDR serialization (`xdr_to_opaque`) plus SHA256 hashing on every invocation with no caching. The two critical sequential call sites are: (1) `collectClusterFootprintEntriesFromGlobal` which runs on the main thread during `ThreadParallelApplyLedgerState` construction (LedgerManagerImpl.cpp:2444) before async dispatch, and (2) `getReadWriteKeysForStage` which runs on the main thread in `commitChangesFromThreads` (line 555) after parallel execution. Both iterate all transactions' footprint keys per stage, calling `getTTLKey` for each Soroban key regardless of whether the same key was already processed.

### Code Paths Examined

- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey(LedgerKey)`: confirmed unconditional `sha256(xdr::xdr_to_opaque(e))` with no cache, no memoization
- `src/transactions/ParallelApplyUtils.cpp:100-118` — `getReadWriteKeysForStage`: iterates all txs in stage, calls `getTTLKey(lk)` for each RW Soroban key. Called from `commitChangesFromThreads` (line 555), which is sequential on main thread
- `src/transactions/ParallelApplyUtils.cpp:562-607` — `collectClusterFootprintEntriesFromGlobal`: iterates all txs in cluster, calls `getTTLKey(key)` for each Soroban key (RO + RW). Called from `ThreadParallelApplyLedgerState` constructor (line 622), which is constructed on the main thread at LedgerManagerImpl.cpp:2444
- `src/ledger/LedgerManagerImpl.cpp:2441-2450` — `applySorobanStageClustersInParallel`: constructs `ThreadParallelApplyLedgerState` on main thread in a sequential loop before launching async tasks
- `src/transactions/ParallelApplyUtils.cpp:626-659` — `flushRoTTLBumpsInTxWriteFootprint`: calls `getTTLKey(lk)` per RW key per tx, runs on apply threads (parallel)
- `src/transactions/ParallelApplyUtils.cpp:148-162` — `buildRoTTLSet`: calls `getTTLKey(ro)` per RO key per tx, runs on apply threads (parallel)
- `src/ledger/InMemorySorobanState.h:242-247` — `InternalContractDataMapEntry(LedgerKey)` constructor: calls `getTTLKey(ledgerKey)` for CONTRACT_DATA lookups. Called from `InMemorySorobanState::get` (line 212) when entries aren't in thread map
- `src/transactions/ParallelApplyUtils.cpp:700-735` — `getLiveEntryOpt`: falls through to `mInMemorySorobanState.get(key)` for Soroban keys not in thread entry map (common, since global map only has classic-modified entries)

### Findings

1. **The inefficiency is real**: `getTTLKey` performs `xdr_to_opaque` (heap allocation + serialization) followed by SHA256 on every call. No caching exists anywhere in the codebase for this function.

2. **Sequential overhead confirmed**: The two main sequential call sites (`collectClusterFootprintEntriesFromGlobal` and `getReadWriteKeysForStage`) both run on the main thread. Critically, `collectClusterFootprintEntriesFromGlobal` is called inside the `ThreadParallelApplyLedgerState` constructor which is constructed in a sequential loop at LedgerManagerImpl.cpp:2441-2450 *before* the async task is launched. This means ALL cluster setup runs sequentially, not in parallel.

3. **Redundancy quantified**: For keys that appear in multiple transactions' footprints (common for shared contract state), `getTTLKey` is computed once per tx per call site. With 200 txs sharing ~100 unique Soroban keys across 20-key footprints, redundancy is ~3x-5x.

4. **Parallel overhead exists but is secondary**: `buildRoTTLSet` and `flushRoTTLBumpsInTxWriteFootprint` run on apply threads, contributing parallel overhead. Additionally, `InMemorySorobanState::get` calls `getTTLKey` via the `InternalContractDataMapEntry` constructor for every CONTRACT_DATA lookup not in the thread map — and since `collectClusterFootprintEntriesFromGlobal` only copies entries from the global map (which mostly has classic entries), most first-access Soroban lookups go through InMemorySorobanState.

5. **Pure function — caching is safe**: `getTTLKey` is a pure function (deterministic, no side effects). The SHA256 of a serialized LedgerKey never changes. Caching introduces no correctness risk, no cache invalidation needed.

6. **No existing optimizations**: No caching, pooling, or memoization exists for `getTTLKey` anywhere in the codebase. The `InternalContractDataMapEntry::ValueEntry::copyKey()` also recomputes `getTTLKey` on every call (though this is mitigated by libstdc++ hash caching in `unordered_set`).

### PoC Guidance

- **Target code**: `src/transactions/ParallelApplyUtils.cpp` — add a `std::unordered_map<LedgerKey, LedgerKey> ttlKeyCache` local to the sequential call sites. Specifically:
  - In `getReadWriteKeysForStage`: create a local cache, replace `getTTLKey(lk)` with a cache-consulting wrapper
  - In `collectClusterFootprintEntriesFromGlobal`: same pattern with a local cache
  - For parallel call sites (`buildRoTTLSet`, `flushRoTTLBumpsInTxWriteFootprint`): add per-thread caches in `ThreadParallelApplyLedgerState` (a member `mTTLKeyCache`)
- **Change description**: Create a `getTTLKeyCached(LedgerKey const&, UnorderedMap<LedgerKey, LedgerKey>&)` helper that checks the cache before computing. Use it at all hot call sites. Alternatively, store computed TTL keys in `ThreadParallelApplyLedgerState` so they survive across function calls within the same cluster.
- **Correctness check**: Existing tests for parallel apply should all pass unmodified — `[tx][soroban][parallelapply]` tag covers the parallel path. Also run `[tx][soroban]` tests for the sequential Soroban path.
- **Benchmark focus**: Run apply-load benchmark with T=8 and soroswap or custom_token workloads. Measure median and p99 ledger apply time. Expected improvement: ~5-8% reduction in sequential overhead, translating to ~3-6% total wall time improvement at T=8.

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-09
**PoC by**: claude-opus-4-6, high

### Changes Made

- **`src/transactions/ParallelApplyUtils.cpp`** (anonymous namespace, ~line 99): Added `getTTLKeyCached(LedgerKey const&, UnorderedMap<LedgerKey, LedgerKey>&)` helper that performs cache lookup via `try_emplace` before falling through to `getTTLKey`. Returns a `const&` to the cached result to avoid copies.

- **`src/transactions/ParallelApplyUtils.cpp:getReadWriteKeysForStage`** (~line 112): Added a local `UnorderedMap<LedgerKey, LedgerKey> ttlKeyCache` and replaced `getTTLKey(lk)` with `getTTLKeyCached(lk, ttlKeyCache)`. This eliminates redundant hashing across transactions sharing RW keys within a stage.

- **`src/transactions/ParallelApplyUtils.h:ThreadParallelApplyLedgerState`** (~line 115): Added `mutable UnorderedMap<LedgerKey, LedgerKey> mTTLKeyCache` member. This cache persists across all function calls within a cluster's lifetime, covering `collectClusterFootprintEntriesFromGlobal`, `flushRoTTLBumpsInTxWriteFootprint`, `buildRoTTLSet`, and `commitChangesFromSuccessfulTx`.

- **`src/transactions/ParallelApplyUtils.cpp:collectClusterFootprintEntriesFromGlobal`** (~line 602): Replaced `getTTLKey(key)` with `getTTLKeyCached(key, mTTLKeyCache)`.

- **`src/transactions/ParallelApplyUtils.cpp:flushRoTTLBumpsInTxWriteFootprint`** (~line 639): Replaced `getTTLKey(lk)` with `getTTLKeyCached(lk, mTTLKeyCache)`.

- **`src/transactions/ParallelApplyUtils.cpp:buildRoTTLSet`** (~line 149): Changed signature to accept `UnorderedMap<LedgerKey, LedgerKey>& ttlKeyCache` parameter, replaced `getTTLKey(ro)` with `getTTLKeyCached(ro, ttlKeyCache)`.

- **`src/transactions/ParallelApplyUtils.cpp:commitChangesFromSuccessfulTx`** (~line 835): Updated `buildRoTTLSet` call to pass `mTTLKeyCache`.

### Demonstration

The optimization eliminates redundant XDR serialization + SHA256 hashing in `getTTLKey` by introducing a per-scope cache (`UnorderedMap<LedgerKey, LedgerKey>`) at two levels: a local cache in the sequential `getReadWriteKeysForStage` function, and a per-cluster member cache in `ThreadParallelApplyLedgerState` that persists across `collectClusterFootprintEntriesFromGlobal`, `buildRoTTLSet`, `flushRoTTLBumpsInTxWriteFootprint`, and `commitChangesFromSuccessfulTx`. For workloads with shared contract keys across transactions, this should reduce redundant `getTTLKey` calls by ~3-5x, removing ~7ms of serial overhead per ledger in T=8 scenarios.

### Test Results

- All 21 tests in `[parallelapply]` passed (2,627,573 assertions)
- All 68 tests in `[tx][soroban]` passed (49,311 assertions)
- Full test suite (`make check`) passed: all partitioned tests + selftest-nopg + check-nondet

---

## Final Review

**Verdict**: REJECTED
**Date**: 2026-04-09
**Final review by**: gpt-5.4, high
**Failed At**: final-review

### Adversarial Analysis

1. **Exercises claimed inefficiency**: YES — the changed call sites are exactly the parallel-apply setup / teardown paths identified in the hypothesis, so the benchmark does measure the intended optimization target.
2. **Realistic preconditions**: YES — the fixed apply-load matrix repeatedly executes these paths under normal Soroban benchmark workloads.
3. **Inefficiency vs by-design**: NOT DEMONSTRATED AS A NET INEFFICIENCY — although `getTTLKey` does repeated SHA256 work, the replacement adds `unordered_map<LedgerKey, LedgerKey>` lookups, allocations, and equality checks keyed by full `LedgerKey` objects. `std::hash<LedgerKey>` already hashes contract fields and SCVal content (`src/ledger/LedgerHashUtils.h`), so the cache is not a near-free memo table.
4. **Benchmark impact**: FAIL — independent results against `ai-summary/baseline.csv` regress four of six scenarios materially:
   - `sac,TX=6400,T=1`: p50 **-14.96%**, p95 **-12.92%**, p99 **-19.51%**
   - `sac,TX=6400,T=8`: p50 **-21.54%**, p95 **-20.63%**, p99 **-20.16%**
   - `custom_token,TX=3000,T=1`: p50 **-9.58%**, p95 **-10.17%**, p99 **-9.30%**
   - `custom_token,TX=3000,T=8`: p50 **-14.81%**, p95 **-10.60%**, p99 **-3.04%**
   Only `soroswap,TX=1600,T=1` improved materially, and `soroswap,TX=1600,T=8` was effectively flat / slightly worse.
5. **In scope**: YES — the PoC stays within the C++ transaction-ledger parallel-apply code.
6. **Benchmark methodology**: CORRECT — independent rebuild, full `make check`, then the project-provided `scripts/run_apply_load_matrix.py` against the provided baseline CSV.
7. **Alternative explanations**: THE REGRESSION MATCHES THE MECHANISM — the saved SHA256 work is outweighed by hash-table churn on complex `LedgerKey` keys. The data is consistent with cache overhead dominating any reuse benefit on these workloads.
8. **Novelty**: IRRELEVANT — the measured performance claim does not hold.

### Rejection Reason

Independent benchmarking shows this optimization is a net slowdown, not an improvement. The PoC removes some redundant `getTTLKey` recomputation, but replacing it with `unordered_map<LedgerKey, LedgerKey>` memoization adds enough key-hashing, equality, allocation, and cache-management overhead to make the real apply-load workloads slower overall.

### Failed Checks

- 3
- 4
- 7
