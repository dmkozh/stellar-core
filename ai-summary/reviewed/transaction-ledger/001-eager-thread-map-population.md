# H001: Eagerly Populate ThreadParallelApplyLedgerState from InMemorySorobanState

**Date**: 2026-04-08
**Subsystem**: transaction-ledger (transactions/ParallelApplyUtils, ledger/InMemorySorobanState)
**Severity**: Low
**Impact**: 5-10% improvement on T=8 Soroban scenarios by reducing per-thread entry lookup overhead
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `ThreadParallelApplyLedgerState` is constructed for a cluster, all
footprint entries (including those from `InMemorySorobanState`) should be
loaded into `mThreadEntryMap` so that subsequent `getLiveEntryOpt` calls
during per-transaction processing are O(1) hash map lookups without
falling through to `InMemorySorobanState`.

## Mechanism

`collectClusterFootprintEntriesFromGlobal` only loads entries found in the
`GlobalParallelApplyEntryMap` into `mThreadEntryMap`. Soroban entries not
modified by the sequential phase (the majority) are left unloaded. Since
`ThreadParallelApplyLedgerState::getLiveEntryOpt` is a const function that
does NOT insert results into `mThreadEntryMap`, each subsequent call for the
same key repeats the full lookup chain: `mThreadEntryMap.find` → miss →
`InMemorySorobanState::get(key)`.

For CONTRACT_DATA entries, each `InMemorySorobanState::get` call constructs
an `InternalContractDataMapEntry(ledgerKey)` which calls
`getTTLKey(ledgerKey)` → `sha256(xdr::xdr_to_opaque(e))`, a ~700ns
SHA256 + XDR serialization per lookup.

For each readWrite entry NOT in `mThreadEntryMap`, there are 4 redundant
lookups per tx:
1. `addReads` during footprint setup
2. `TxParallelApplyLedgerState::upsertEntry` (checks existence)
3. `setEffectsDeltaFromSuccessfulTx` (gets previous state for delta)
4. `commitChangesFromSuccessfulTx` (gets previous state for commit)

For the first tx in a cluster with 20 Soroban entries, this is ~80
redundant `InMemorySorobanState` lookups × ~700ns = ~56µs overhead. Across
8 threads processing different clusters, the cumulative overhead per ledger
scales with total unique entries.

## Trigger

Run the apply-load benchmark with T=8 threads and soroswap transactions
(which have large, varied footprints). Compare wall-clock time of
`applySorobanStageClustersInParallel` before and after the optimization.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:562-607` — `collectClusterFootprintEntriesFromGlobal`: only fetches from global map, should also load from `mInMemorySorobanState`
- `src/transactions/ParallelApplyUtils.cpp:699-735` — `ThreadParallelApplyLedgerState::getLiveEntryOpt`: falls through to InMemorySorobanState without caching
- `src/transactions/ParallelApplyUtils.cpp:790-829` — `setEffectsDeltaFromSuccessfulTx`: calls `getLiveEntryOpt` for each entry (redundant)
- `src/transactions/ParallelApplyUtils.cpp:832-843` — `commitChangesFromSuccessfulTx`: calls `getLiveEntryOpt` for each entry (redundant)

## Evidence

- `getLiveEntryOpt` (line 699) is declared `const` and does NOT insert into `mThreadEntryMap`
- `InMemorySorobanState::get` for CONTRACT_DATA (line 211-212) constructs `InternalContractDataMapEntry(ledgerKey)` which calls `getTTLKey` → SHA256
- `setEffectsDeltaFromSuccessfulTx` (line 797) and `commitChangesFromSuccessfulTx` (line 836) both iterate modified entries and call `getLiveEntryOpt` on the same keys
- `collectClusterFootprintEntriesFromGlobal` (line 577-607) only checks `globalEntryMap.find()`, not InMemorySorobanState

## Anti-Evidence

- For entries that ARE in `mThreadEntryMap` (from globalEntryMap or from previous tx commits in the same cluster), lookups are already O(1)
- After the first tx in a cluster commits, many entries would be in `mThreadEntryMap` for subsequent txs
- InMemorySorobanState lookups for CONTRACT_CODE use `unordered_map<uint256>` (fast direct hash, no SHA256)
- The VM execution time dominates per-tx cost, making the lookup overhead a small fraction

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete lookup chain from `ThreadParallelApplyLedgerState::getLiveEntryOpt` through `InMemorySorobanState::get` to confirm that CONTRACT_DATA lookups construct `InternalContractDataMapEntry(ledgerKey)` which calls `getTTLKey → sha256(xdr::xdr_to_opaque(e))` on every invocation. Verified that `getLiveEntryOpt` is const and does not cache results. Confirmed that `commitChangeFromSuccessfulTx` only inserts modified entries into `mThreadEntryMap`, leaving readOnly footprint entries uncached across all txs in a cluster. The inefficiency is real but VM execution dominates per-tx cost.

### Code Paths Examined

- `src/transactions/ParallelApplyUtils.cpp:562-607` — `collectClusterFootprintEntriesFromGlobal`: confirmed only checks `globalEntryMap.find()`, does not load from InMemorySorobanState for cache misses
- `src/transactions/ParallelApplyUtils.cpp:699-735` — `ThreadParallelApplyLedgerState::getLiveEntryOpt`: confirmed const, no caching of InMemorySorobanState results; falls through for every miss
- `src/transactions/ParallelApplyUtils.cpp:760-787` — `commitChangeFromSuccessfulTx`: calls `getLiveEntryOpt` for old value, then `upsertEntry`/`eraseEntry` — only caches entries that were MODIFIED, not readOnly entries
- `src/transactions/ParallelApplyUtils.cpp:790-829` — `setEffectsDeltaFromSuccessfulTx`: calls `getLiveEntryOpt` for same keys as `commitChangesFromSuccessfulTx`, both before mThreadEntryMap is populated
- `src/transactions/ParallelApplyUtils.cpp:886-904` — `TxParallelApplyLedgerState::getLiveEntryOpt`: falls through to `mThreadState.getLiveEntryOpt` when `mTxEntryMap` misses
- `src/transactions/ParallelApplyUtils.cpp:907-951` — `TxParallelApplyLedgerState::upsertEntry`: calls `getLiveEntryOpt` to check existence — another fallthrough to InMemorySorobanState
- `src/ledger/InMemorySorobanState.cpp:205-236` — `InMemorySorobanState::get`: for CONTRACT_DATA, constructs `InternalContractDataMapEntry(ledgerKey)` → heap alloc of `QueryKey` + `getTTLKey` → SHA256; for CONTRACT_CODE, directly calls `getTTLKey` → SHA256
- `src/ledger/InMemorySorobanState.h:242-258` — `InternalContractDataMapEntry(LedgerKey)`: for CONTRACT_DATA calls `getTTLKey(ledgerKey)` (SHA256); for TTL uses `keyHash` directly (no SHA256)
- `src/ledger/LedgerTypeUtils.cpp:31-37` — `getTTLKey(LedgerKey)`: confirmed `sha256(xdr::xdr_to_opaque(e))` — XDR serialization + SHA256 on every call

### Findings

**The inefficiency is real but the severity is Informational, not Low.**

The hypothesis correctly identifies that:
1. `getLiveEntryOpt` is const and does not cache results in `mThreadEntryMap` ✓
2. `InMemorySorobanState::get` for CONTRACT_DATA computes SHA256 via `getTTLKey` on every call ✓
3. The same key is looked up multiple times per tx (up to 4× for readWrite entries on the first tx) ✓
4. ReadOnly footprint entries are NEVER cached in `mThreadEntryMap`, so every tx in the cluster pays the SHA256 cost ✓

**Detailed per-entry cost analysis:**

For a readWrite CONTRACT_DATA entry NOT in `mThreadEntryMap` (first tx in cluster):
- Host read → Tx::getLiveEntryOpt → Thread::getLiveEntryOpt → InMemorySorobanState::get → SHA256 (~900ns with heap alloc)
- Tx::upsertEntry → Tx::getLiveEntryOpt → Thread::getLiveEntryOpt → InMemorySorobanState::get → SHA256
- setEffectsDelta → Thread::getLiveEntryOpt → InMemorySorobanState::get → SHA256
- commitChanges → Thread::getLiveEntryOpt → InMemorySorobanState::get → SHA256
- Total: 4 × ~900ns = ~3.6µs per entry

For subsequent txs in the cluster, modified entries are cached in `mThreadEntryMap` (0 SHA256 cost).

For readOnly CONTRACT_DATA entries (ALL txs in cluster):
- Host read only: 1 × ~900ns per entry per tx (never cached)

**Realistic impact estimate:**

Scenario: T=8 threads, 100 Soroban txs, ~12 txs per cluster, 20 footprint entries per tx (10 CONTRACT_DATA, 5 readWrite + 5 readOnly CONTRACT_DATA):
- First tx readWrite: 5 × 4 SHA256 = 20 SHA256 ops = ~18µs
- All 12 txs readOnly: 5 × 12 × 1 SHA256 = 60 SHA256 ops = ~54µs
- Total per cluster: ~72µs wall-clock
- VM execution per cluster: 12 txs × 5-50ms = 60-600ms
- **Overhead: 0.01-0.12% of per-cluster time**

This is well below the 5% threshold for Low severity. The inefficiency is real and the fix is correct, but VM execution time dominates by 3-4 orders of magnitude.

**Severity downgrade rationale:** The hypothesis claims "5-10% improvement" but the actual overhead is <0.2% of cluster processing time. Informational is the correct severity — the finding is real but has minimal practical impact.

### PoC Guidance

- **Target code**: `src/transactions/ParallelApplyUtils.cpp:562-607` — extend `collectClusterFootprintEntriesFromGlobal` to also load entries from `mInMemorySorobanState` (and `mLCLSnapshot` for non-Soroban types) for keys not found in `globalEntryMap`
- **Change description**: After the `fetchFromGlobal` lambda fails to find a key in globalEntryMap, call `mInMemorySorobanState.get(key)` (for Soroban types) or `mLCLSnapshot.loadLiveEntry(key)` (for non-Soroban types) and insert the result into `mThreadEntryMap` as a clean entry via `ThreadParallelApplyEntry::clean()`
- **Correctness check**: Existing parallel apply tests (`[parallelapply]` tag) cover this code path. The change preserves the clean/dirty tracking invariant since eagerly-loaded entries are marked clean. Verify that `commitChangesFromThreads` still only commits dirty entries.
- **Benchmark focus**: Measure `applySorobanStageClustersInParallel` wall-clock time with T=8 threads and high-footprint Soroban txs. Expected improvement: <0.2% — likely not measurable above noise. Consider microbenchmarking `getLiveEntryOpt` call count and time in isolation.
