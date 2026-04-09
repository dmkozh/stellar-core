# H007: Redundant SorobanNetworkConfig Loading — Four Loads Per Ledger Close When Two Suffice

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Low
**Impact**: CPU reduction per ledger close from eliminating redundant config entry lookups
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

During a normal (non-upgrade) ledger close, the Soroban network configuration should be loaded from the LedgerTxn at most twice: once before transaction execution (for parallel apply) and once after (for final state update and InMemorySorobanState). Each `loadFromLedger()` call performs 15-17 individual `LedgerSnapshot::load()` calls, each constructing a `LedgerKey`, searching the LedgerTxn chain via hash map lookups, and extracting the config value via XDR deserialization.

## Mechanism

During a single ledger close, `SorobanNetworkConfig::loadFromLedger()` is called **four times** on the normal (non-upgrade) code path:

1. **`applyTransactions:2656`** — Loads config before parallel apply. Result passed to `applySorobanStages` and used throughout parallel execution. **Necessary.**

2. **`BucketManager::resolveBackgroundEvictionScan:1191`** — Called from `finalizeLedgerTxnChanges:2961`. Loads config internally to validate eviction candidates. **Necessary, but redundant with #4 on non-upgrade ledgers.**

3. **`finalizeLedgerTxnChanges:2959`** — Loads config into a local variable `sorobanConfig` that is **never read**. The eviction scan at line 2961 loads its own copy internally. This is pure dead code — the variable goes out of scope at line 3029 without being consumed. **Unnecessary — dead code.**

4. **`finalizeLedgerTxnChanges:3037`** — Loads `finalSorobanConfig` used for `updateInMemorySorobanState` and returned to caller. **Necessary.**

On a non-upgrade ledger (the overwhelming majority of ledgers), the LedgerTxn state is identical for loads #1-#4, so all four produce the same `SorobanNetworkConfig` value. Loads #2 and #3 are eliminable: #3 is dead code, and #2 could reuse #4's result if passed as a parameter.

Each `loadFromLedger` performs 15-17 hash map lookups through the LedgerTxn chain (one per config setting: max contract size, compute settings, ledger access settings, historical settings, events, bandwidth, CPU cost params, mem cost params, state archival, execution lanes, live state size window, eviction iterator, plus V23+ settings). At ~0.5-1μs per lookup, each full load costs ~10-20μs, and the two redundant loads add ~20-40μs per ledger.

However, the individual `loadFromLedger` calls also involve XDR object construction and assignment for each config field, which adds to the per-call cost. More significantly, the LedgerSnapshot construction at line 2959 (via `LedgerSnapshot(ltx)` at NetworkConfig.cpp:1794) creates a temporary object with virtual function dispatch for each `load()` call.

## Trigger

Run any Soroban apply-load benchmark. Profile `finalizeLedgerTxnChanges` and look for the `loadFromLedger` calls. The dead code at line 2959 should be visible as pure wasted CPU.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2959` — Dead `loadFromLedger` call (result never used)
- `src/ledger/LedgerManagerImpl.cpp:3036-3037` — Second necessary `loadFromLedger` producing `finalSorobanConfig`
- `src/bucket/BucketManager.cpp:1191` — Third `loadFromLedger` inside eviction scan
- `src/ledger/NetworkConfig.cpp:1754-1789` — `loadFromLedger` performing 15-17 individual loads
- `src/ledger/LedgerManagerImpl.cpp:2656` — First necessary `loadFromLedger` in `applyTransactions`

## Evidence

1. The variable `auto sorobanConfig = SorobanNetworkConfig::loadFromLedger(ltx)` at line 2959 is assigned but never read between lines 2959-3029 (its scope). A `grep` for `sorobanConfig` in that range returns only the assignment.
2. `resolveBackgroundEvictionScan` at BucketManager.cpp:1191 independently calls `loadFromLedger(ls)` without receiving the caller's config.
3. `loadFromLedger` at NetworkConfig.cpp:1754 shows 15-17 sub-load methods called sequentially.
4. On non-upgrade ledgers (>99.9% of ledgers), the config at line 2959 would be identical to line 3037 since no upgrades modify config entries between those points.

## Anti-Evidence

1. The per-load cost is ~10-20μs. Eliminating 2 redundant loads saves ~20-40μs. With ledger close times of 200-500ms, this is <0.02% improvement — well below the Low severity threshold of 5%.
2. The dead code at line 2959 was likely intentional at some point (perhaps the eviction scan signature used to take a config parameter) and removing it is a trivial cleanup, not a performance optimization.
3. Passing the config between `BucketManager::resolveBackgroundEvictionScan` and the caller would change the BucketManager API, adding coupling.
4. The compiler may partially optimize these lookups if the LedgerTxn chain is hot in L1 cache from the preceding transaction execution.
