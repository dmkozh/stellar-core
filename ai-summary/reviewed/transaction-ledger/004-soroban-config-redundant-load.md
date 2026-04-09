# H004: Eliminate Redundant SorobanNetworkConfig::loadFromLedger in applyTransactions

**Date**: 2026-04-08
**Subsystem**: transaction-ledger (ledger/LedgerManagerImpl, ledger/NetworkConfig)
**Severity**: Low
**Impact**: 5-8% reduction in serial apply overhead by eliminating ~19 individual ledger entry lookups
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`applyTransactions` should use the already-cached `SorobanNetworkConfig`
from the LCL state (available via `getLastClosedSorobanNetworkConfig()` or
from `mApplyState`) instead of reloading it from the LedgerTxn. The config
settings are immutable between the LCL and tx application — only protocol
upgrades change them, and upgrades are applied AFTER `applyTransactions`.

## Mechanism

In `LedgerManagerImpl::applyTransactions` (line 2651-2657):
```cpp
sorobanConfig = std::make_optional(SorobanNetworkConfig::loadFromLedger(ltx));
```

`SorobanNetworkConfig::loadFromLedger` (NetworkConfig.cpp:1754-1789)
performs 15-19 individual entry loads from a `LedgerSnapshot`:
- `loadMaxContractSize`, `loadMaxContractDataKeySize`,
  `loadMaxContractDataEntrySize`, `loadComputeSettings`,
  `loadLedgerAccessSettings`, `loadHistoricalSettings`,
  `loadContractEventsSettings`, `loadBandwidthSettings`,
  `loadCpuCostParams`, `loadMemCostParams`, `loadStateArchivalSettings`,
  `loadExecutionLanesSettings`, `loadLiveSorobanStateSizeWindow`,
  `loadEvictionIterator`
- Plus for v23+: `loadParallelComputeConfig`, `loadLedgerCostExtConfig`,
  `loadSCPTimingConfig`
- Plus for v26+: `loadFrozenLedgerKeys`, `loadFreezeBypassTxs`

Each `load` constructs a `LedgerKey(CONFIG_SETTING)`, queries the
`LedgerSnapshot(ltx)` which traverses: ltx entry map → LedgerTxnRoot
cache → BucketList. CONFIG_SETTING entries are rarely in the ltx entry
map (only fee/seqnum changes are there), so each lookup falls through to
the root cache or BucketList.

This runs sequentially on the apply thread, before any parallel execution
begins. The same config was already loaded and cached when the LCL was
established. The config cannot have changed because:
1. `processFeesSeqNums` only modifies account entries (fees, seq nums)
2. No classic or Soroban tx can modify CONFIG_SETTING entries
3. Protocol upgrades are applied AFTER `applyTransactions` returns

## Trigger

Run the apply-load benchmark with any Soroban scenario. Profile the time
spent in `SorobanNetworkConfig::loadFromLedger` during `applyTransactions`
using Tracy or perf.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2651-2657` — loads config in `applyTransactions`
- `src/ledger/NetworkConfig.cpp:1754-1789` — `loadFromLedger`: 15-19 individual entry loads
- `src/ledger/LedgerManagerImpl.cpp:820-828` — `getLastClosedSorobanNetworkConfig`: returns cached config

## Evidence

- `loadFromLedger` (NetworkConfig.cpp:1754-1789) performs 15-19 sequential entry loads, each creating a LedgerKey and querying the snapshot
- CONFIG_SETTING entries are NOT modified by `processFeesSeqNums` (line 2219-2337), which only touches account entries
- Protocol upgrades happen AFTER `applyTransactions` (LedgerManagerImpl.cpp:1664-1712)
- The cached config is already available: `mLastClosedLedgerState->getSorobanNetworkConfig()` and through `mApplyState.copyLedgerStateSnapshot()`
- `applySorobanStages` already receives the config as a parameter (line 2537), showing it's passed by reference downstream

## Anti-Evidence

- The LCL config is from the PREVIOUS ledger; during the current apply, the config is technically "as of the current ltx". But since no operations modify config entries, they are identical.
- Using the cached config requires ensuring thread safety of access to `mLastClosedLedgerState` from the apply thread. However, `mLastClosedLedgerState` is updated only by `advanceLedgerStateAndPublish` (main thread), which runs AFTER `applyLedger` completes.
- If `allBucketsInMemory()` is true, BucketList lookups are fast (~100ns), reducing the per-entry cost
- The total overhead (~19 × 1-5µs = 19-95µs) is small relative to total apply time
- There may be subtle scenarios (startup, catchup) where the LCL config hasn't been initialized yet, requiring a fallback path

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full lookup path from `SorobanNetworkConfig::loadFromLedger(ltx)` at LedgerManagerImpl.cpp:2655 through `LedgerSnapshot(ltx)` → `LedgerTxnReadOnly::load` → `LedgerTxn::loadWithoutRecord` → `getNewestVersion` hierarchy traversal → `LedgerTxnRoot::Impl::getNewestVersion`. Confirmed that CONFIG_SETTING entries miss the ltx entry maps (only accounts from processFeesSeqNums), miss the `mEntryCache` (cleared at end of previous commit at LedgerTxn.cpp:2974), are not in `InMemorySorobanState` (only CONTRACT_DATA/CONTRACT_CODE/TTL per InMemorySorobanState.cpp:147), and fall through to BucketList lookup via `loadLiveEntry`. Confirmed the cached config in `mApplyState.getLedgerState()->getSorobanConfig()` is always available and identical for post-Soroban ledgers.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:2607-2657` — `applyTransactions` entry, config load at line 2655-2656
- `src/ledger/NetworkConfig.cpp:1754-1795` — `loadFromLedger` creates `LedgerSnapshot(ltx)`, performs 15-19 individual loads
- `src/ledger/LedgerStateSnapshot.cpp:239-241` — `LedgerSnapshot(AbstractLedgerTxn&)` wraps ltx in `LedgerTxnReadOnly`
- `src/ledger/LedgerTxn.cpp:2161-2195` — `loadWithoutRecord` → `getNewestVersion` hierarchy traversal
- `src/ledger/LedgerTxn.cpp:1731-1738` — `getNewestVersion` checks mEntry then recurses to parent
- `src/ledger/LedgerTxn.cpp:3614-3673` — `LedgerTxnRoot::Impl::getNewestVersion`: cache miss → not InMemorySorobanState → `loadLiveEntry` from BucketList snapshot
- `src/ledger/LedgerTxn.cpp:2972-2974` — `mEntryCache.clear()` in commit path, confirming cache is empty at start of new ledger
- `src/ledger/InMemorySorobanState.cpp:145-149` — `isInMemoryType` excludes CONFIG_SETTING
- `src/ledger/LedgerManagerImpl.cpp:2219-2337` — `processFeesSeqNums` only modifies accounts and MAX_SEQ_NUM_TO_APPLY
- `src/ledger/LedgerManagerImpl.cpp:1660-1712` — Upgrades applied AFTER `applyTransactions`
- `src/ledger/LedgerManagerImpl.cpp:2180-2195` — `ApplyState::getLedgerState()` and `copyLedgerStateSnapshot()` accessible from apply thread
- `src/ledger/LedgerManagerImpl.cpp:2102-2130` — `buildLedgerState` loads sorobanConfig from BucketList when nullopt, ensuring LCL always has config
- `src/ledger/LedgerManagerImpl.cpp:1893-1928` — `setLastClosedLedger` (catchup path) also populates config via `buildLedgerState`
- `src/ledger/LedgerManagerImpl.cpp:820-831` — `getLastClosedSorobanNetworkConfig` asserts main thread; apply thread must use `mApplyState.getLedgerState()`

### Findings

**The optimization is correct and safe.** The mechanism is valid:

1. **Immutability confirmed**: CONFIG_SETTING entries cannot change between LCL establishment and `applyTransactions`. `processFeesSeqNums` only touches accounts (line 2256-2257: `tx->processFeeSeqNum`). No classic or Soroban tx modifies CONFIG_SETTING. Upgrades run after (line 1664).

2. **Cached config always available**: For any Soroban-protocol ledger where line 2652 is true, the LCL's `mApplyState.getLedgerState()` has the config. This is because:
   - Normal close: `buildLedgerState` (line 2113-2120) loads config when nullopt
   - Catchup: `setLastClosedLedger` (line 1911-1912) passes nullopt but `buildLedgerState` fills it
   - Soroban upgrade ledger: line 2652 check is FALSE (header not yet upgraded), so the code path isn't reached

3. **Thread safety**: `mApplyState.getLedgerState()` returns the apply state's own `CompleteConstLedgerStatePtr`, designed for apply-thread access. The config is immutable within the `CompleteConstLedgerState` object.

4. **Impact is negligible**: Each CONFIG_SETTING lookup traverses 2 LedgerTxn mEntry misses, 1 LedgerTxnRoot cache miss (cleared at LedgerTxn.cpp:2974), and hits BucketList via `loadLiveEntry`. Cost per lookup:
   - `allBucketsInMemory() == true`: ~100-200ns (in-memory hash table)
   - `allBucketsInMemory() == false`: ~1-10µs (disk/page-cache I/O)
   - Total for 19 entries: 2-190µs
   - Total apply time: 50-500ms
   - Savings: 0.0004% to 0.38% of total apply time

**Severity downgraded from Low to Informational** because the savings (~2-190µs) are far below 5% of any benchmark scenario. The original claim of "5-8% reduction in serial apply overhead" conflates the tiny config-load cost with total serial overhead. Even considering only the pre-parallel setup (~1-5ms), the savings are at most ~4-10% of setup but <0.04% end-to-end. No benchmark would show a measurable change.

### PoC Guidance

- **Target code**: `src/ledger/LedgerManagerImpl.cpp:2651-2657` — replace `SorobanNetworkConfig::loadFromLedger(ltx)` with `mApplyState.getLedgerState()->getSorobanConfig()`
- **Change description**: Replace the 15-19 BucketList lookups with a reference to the already-cached config from the apply state. The change is a single-line replacement. Add `releaseAssert(mApplyState.getLedgerState()->hasSorobanConfig())` before the access for safety.
- **Correctness check**: Existing Soroban tests (tag `[soroban]`) cover the `applyTransactions` path. Also run `[tx]` tests. The `ApplyLoadTest` benchmark would also exercise this path.
- **Benchmark focus**: Profile `SorobanNetworkConfig::loadFromLedger` time within `applyTransactions` using Tracy. Expect elimination of ~2-190µs per ledger, which would be visible in Tracy flamegraph but NOT in aggregate benchmark results (< 0.4% improvement).
