# H005: Replace Full Classic-Footprint Probing with Modified-Key Seeding

**Date**: 2026-04-09
**Subsystem**: transaction-ledger (transactions/ParallelApplyUtils, ledger/LedgerTxn)
**Severity**: Low
**Impact**: Serial pre-parallel setup optimization
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

After `preParallelApply` has updated fee-source accounts and sequence numbers,
parallel setup should seed `mGlobalEntryMap` from the small set of classic
entries that were actually modified below root in the current ledger. Classic
footprint keys that were not modified yet should be left alone and loaded lazily
from the per-thread snapshot when first accessed.

## Mechanism

`preParallelApplyAndCollectModifiedClassicEntries` currently makes a second pass
over every Soroban tx footprint and calls `ltx.getNewestVersionBelowRoot(lk)`
for every non-Soroban key, even though only a subset of those keys can possibly
exist below root at that point. In apply-load workloads with many classic
account / trustline footprint entries, this creates a serial O(total classic
footprint keys) miss-heavy probe phase before any worker thread starts, when the
same result could be obtained from the already-maintained modified-key set in
`LedgerTxn`.

## Trigger

Run the apply-load benchmark for `sac` or `custom_token` with `T=8`, and
profile `GlobalParallelApplyLedgerState::preParallelApplyAndCollectModifiedClassicEntries`.
The hypothesis is strongest when footprints contain many classic
ACCOUNT/TRUSTLINE keys but only fee-source accounts were modified before the
parallel phase.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:333-355` — `fetchInMemoryClassicEntries` probes `ltx.getNewestVersionBelowRoot` for every non-Soroban footprint key
- `src/transactions/ParallelApplyUtils.cpp:363-384` — second pass over all tx footprints after `preParallelApply`
- `src/ledger/LedgerTxn.cpp:1704-1721` — `getAllKeysWithoutSealing` exposes the exact below-root modified-key set

## Evidence

- The code comment says only classic entries modified earlier in the ledger need
  to be copied into `mGlobalEntryMap`, but the implementation probes *all*
  non-Soroban footprint keys.
- `getNewestVersionBelowRoot` only checks in-memory `LedgerTxn` layers; for an
  untouched classic key it can only miss.
- The miss-heavy second pass is fully serial and runs before
  `applySorobanStageClustersInParallel` launches any worker work.

## Anti-Evidence

- If a workload modifies most classic footprint keys before the parallel phase,
  the miss rate falls and the savings shrink.
- Footprints dominated by Soroban-only keys will not benefit much from this
  change.
- The absolute cost depends on how many classic keys appear in benchmark
  footprints; some scenarios may only see a small single-digit gain.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the full execution path from `applyLedger` through `processFeesSeqNums`,
`applyParallelPhase`, and into `preParallelApplyAndCollectModifiedClassicEntries`.
Examined the LedgerTxn hierarchy (single layer between `ltx` and root) and the
actual footprint composition for all three benchmark workloads (SAC, custom_token,
soroswap). Verified what `getNewestVersionBelowRoot` does on hit and miss, and
what classic keys actually appear in benchmark footprints.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:1478` — `LedgerTxn ltx(root)`, single layer hierarchy
- `src/ledger/LedgerManagerImpl.cpp:1641-1643` — `processFeesSeqNums` charges fees on ALL tx source accounts, committing into `ltx.mEntry`
- `src/transactions/TransactionFrame.cpp:2049-2123` — `commonPreApply` bumps seqnums via `processSeqNum`, also into `ltx.mEntry`
- `src/transactions/ParallelApplyUtils.cpp:333-355` — `fetchInMemoryClassicEntries` lambda probes each non-Soroban footprint key
- `src/ledger/LedgerTxn.cpp:1761-1768` — `getNewestVersionBelowRoot` checks `mEntry` then recurses to parent (root returns `{false, nullptr}`)
- `src/simulation/TxGenerator.cpp:738-812` — SAC individual transfer footprint: readOnly=2 Soroban keys, readWrite=1 ACCOUNT (fee source) + 1 CONTRACT_DATA
- `src/simulation/TxGenerator.cpp:1449-1522` — SAC batch transfer footprint: ALL keys are Soroban (no classic keys)
- `src/simulation/TxGenerator.cpp:815-885` — custom_token footprint: ALL keys are Soroban (no classic keys)

### Why It Failed

The core claim of a "miss-heavy probe phase" is incorrect for the actual
benchmark workloads:

1. **SAC individual transfer**: The only classic key in each footprint is the
   `fromKey` ACCOUNT, which IS the fee-source account. Since `processFeesSeqNums`
   already charged fees on this account (modifying `ltx.mEntry`), every
   `getNewestVersionBelowRoot` probe is a **hit** — not a miss. With 6400 txs,
   there are 6400 classic probes that all find entries immediately in the first
   hash map lookup.

2. **SAC batch transfer**: The footprint contains ONLY Soroban keys
   (CONTRACT_DATA for balance entries). There are **zero** classic key probes.
   The loop simply skips all keys via the `isSorobanEntry` check.

3. **custom_token**: Similarly, the footprint contains ONLY Soroban keys
   (code, instance, two CONTRACT_DATA balance entries). **Zero** classic key
   probes.

Additionally, the proposed alternative (`getAllKeysWithoutSealing`) creates a
full copy of the modified key set (`LedgerKeySet`), which for a 6400-tx ledger
means allocating and inserting ~6400 keys. This cost is comparable to or
exceeds the cost of the current approach's iteration with `isSorobanEntry`
skips. The current code's overhead for the SAC benchmark is ~1.5ms total
(25,600 iterations with 19,200 fast skips + 6,400 hit probes), which is
<0.3% of a typical 500ms ledger apply time.

### Lesson Learned

Before claiming "miss-heavy" probes, verify the actual footprint composition of
benchmark workloads. In Soroban benchmarks (SAC, custom_token, soroswap), the
only classic keys in footprints are fee-source accounts, which are always
pre-modified by `processFeesSeqNums`. Trustline keys do NOT appear in these
footprints. The `isSorobanEntry` skip for Soroban-only keys is extremely cheap
(enum comparison) and does not warrant replacement with a key-set copy.
