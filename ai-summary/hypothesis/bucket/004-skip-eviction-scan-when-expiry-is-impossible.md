# H004: Apply-load still runs the eviction scanner on ledgers where nothing can expire

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: Medium
**Impact**: background CPU/I/O and ledger-close synchronization
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When the active Soroban archival settings make expiry impossible for the benchmark horizon, bucket eviction should be skipped entirely. In that mode, ledger close should not spawn a background scan or later block on `mEvictionFuture.get()` for a result set that must be empty.

## Mechanism

After every committed Soroban ledger, `LedgerManagerImpl` always starts a background eviction scan, and the next ledger's `resolveBackgroundEvictionScan` always waits for its result. The apply-load harness deliberately sets `minPersistentTTL` and `minTemporaryTTL` to ~1e9 to avoid archival, but bucket code does not exploit that fact: it still scans bucket files and validates empty candidate sets every ledger, burning CPU on a path the benchmark configuration has effectively disabled.

## Trigger

Run any Soroban apply-load benchmark scenario (`sac`, `custom_token`, or `soroswap`) with the stock apply-load upgrade config from `ApplyLoad.cpp`. The stronger the run's parallelism (`T=8`), the more the extra background work competes for CPU with apply threads.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:1837-1847` — unconditionally starts a new background scan after each committed Soroban ledger
- `src/bucket/BucketManager.cpp:startBackgroundEvictionScan:1151-1177` — always packages and dispatches a scan task
- `src/bucket/BucketManager.cpp:resolveBackgroundEvictionScan:1181-1213` — always waits for and validates scan output
- `src/simulation/ApplyLoad.cpp:getUpgradeConfigForTesting/getUpgradeConfigForMaxTPS:123-135,176-200` — benchmark config intentionally makes archival irrelevant

## Evidence

The benchmark config comment says it increases TTL specifically "in order to avoid the state archival." Despite that, the bucket manager still enters the eviction path every ledger, and `resolveBackgroundEvictionScan` eagerly loads Soroban config and joins the future even when no restore/archive activity is expected.

## Anti-Evidence

This optimization likely needs a precise guard derived from archival settings, because normal validator configs absolutely do need continuous scans. If even a subset of benchmark scenarios can still produce expiring entries despite the large TTLs, the win would be smaller or require a narrower predicate.
