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

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the full eviction pipeline: `startBackgroundEvictionScan` posts a scan task to a dedicated eviction background thread (medium priority, separate io_context). The scan reads `evictionScanSize` bytes from bucket files, parses XDR entries, and performs BucketListDB `loadKeys()` for TTL keys. `resolveBackgroundEvictionScan` on the main thread reloads `SorobanNetworkConfig`, blocks on `mEvictionFuture.get()`, validates candidates, and writes back the updated eviction iterator. The background scan starts at the end of ledger N and is resolved at the beginning of ledger N+1's `finalizeLedgerTxnChanges`, giving the background thread a full ledger close cycle to complete.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:1846-1847` — `startBackgroundEvictionScan` called after commit, before state publish
- `src/ledger/LedgerManagerImpl.cpp:2959-2962` — `resolveBackgroundEvictionScan` called inside `finalizeLedgerTxnChanges` of the *next* ledger
- `src/bucket/BucketManager.cpp:1151-1178` — `startBackgroundEvictionScan`: creates packaged_task, posts to dedicated eviction io_context
- `src/bucket/BucketManager.cpp:1181-1291` — `resolveBackgroundEvictionScan`: loads config (cached in LedgerTxn), blocks on future, iterates empty candidates, writes eviction iterator
- `src/bucket/BucketListSnapshot.cpp:601-651` — `scanForEviction`: iterates buckets from eviction iterator position, scans up to `evictionScanSize` bytes
- `src/bucket/BucketListSnapshot.cpp:713-858` — `scanForEvictionInBucket`: opens file stream, reads entries, collects Soroban entries, calls `loadKeys()` for TTL checks
- `src/main/ApplicationImpl.cpp:175-179` — Dedicated eviction thread at medium priority, separate from worker pool
- `src/simulation/ApplyLoad.cpp:123-124` — `getUpgradeConfigForTesting`: `evictionScanSize = 100,000` (100KB)
- `src/simulation/ApplyLoad.cpp:176-177` — `getUpgradeConfigForMaxTPS`: `evictionScanSize = 100` (100 bytes)

### Why It Failed

The inefficiency exists but is **not in a hot path with sufficient per-invocation cost** to produce measurable improvement:

1. **`getUpgradeConfigForMaxTPS` (TPS-maximizing benchmarks)**: `evictionScanSize = 100` bytes — the scan reads 1-2 XDR entries. Background thread completes in microseconds. Main-thread overhead is negligible.

2. **`getUpgradeConfigForTesting` (sac/custom_token/soroswap)**: `evictionScanSize = 100,000` bytes — the scan reads ~50-200 entries from a level-7 bucket. The dedicated eviction thread has a full ledger close cycle (~100-500ms) to complete this ~1-5ms scan. `mEvictionFuture.get()` returns immediately because the scan finishes long before `resolveBackgroundEvictionScan` is called in the next ledger.

3. **Main-thread fixed overhead**: `SorobanNetworkConfig::loadFromLedger` re-reads ~15 config entries (already cached in LedgerTxn from the call at line 2959), `updateEvictionIterator` writes one config setting entry. Total: ~100-500μs per ledger, which is <0.5% of typical ledger close time (100-500ms).

4. **No CPU contention**: The eviction scan runs on a dedicated thread with its own io_context (`mEvictionIOContext`), not competing for slots in the worker thread pool. OS-level CPU contention with 8 worker threads is minimal for a ~1-5ms task.

5. **Simpler alternative exists**: Setting `evictionScanSize = 0` in benchmark configs would make the scan return immediately (the code already handles this: `bytesToScan == 0` → `Loop::COMPLETE`), without requiring any code changes.

The claimed "Medium" severity (10-20% improvement) is not supported. The total per-ledger overhead is well below 1%, far under the 5% threshold for "Low" severity.

### Lesson Learned

When evaluating background eviction scan overhead, check both the scan size *and* the timing relationship between `startBackgroundEvictionScan` (end of ledger N) and `resolveBackgroundEvictionScan` (beginning of ledger N+1). The full ledger close interval gives the background thread ample time to complete, so `mEvictionFuture.get()` rarely blocks. For benchmark-specific optimizations, consider config-level changes (e.g., `evictionScanSize = 0`) before proposing code-level guards.
