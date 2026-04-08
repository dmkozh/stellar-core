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
