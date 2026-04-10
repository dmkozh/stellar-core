# H021: `LedgerTxnRoot::commitChild` Iterates All Entries for Soroban-Only Workloads With Zero SQL-Eligible Entries

**Date**: 2026-04-10
**Subsystem**: ledger (LedgerTxn)
**Severity**: Low
**Impact**: Skip ~50K entry iterations when no SQL-eligible entries (offers) exist
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `LedgerTxnRoot::commitChild` commits the outermost LedgerTxn to the
database (LedgerTxn.cpp:2918-2991), it should skip the entry iteration loop
when no SQL-eligible entry types (currently only OFFERs) were modified. For
pure Soroban workloads (SAC, custom_token, soroswap), zero offers are
modified, so the `while ((bool)iter)` loop at line 2942-2953 iterates ~50K
entries solely to call `bleca.accumulate(iter)` which immediately returns
`false` for every entry.

## Mechanism

`BulkLedgerEntryChangeAccumulator::accumulate` (LedgerTxn.cpp:2877-2895)
checks two conditions per entry:
1. `iter.key().type() != InternalLedgerEntryType::LEDGER_ENTRY` → skip
2. `!LiveBucketIndex::typeNotSupported(type)` → skip (true for CONTRACT_DATA, CONTRACT_CODE, TTL, ACCOUNT)

Only OFFERs pass both checks. In Soroban-only workloads, there are zero
offers. But the loop still iterates all ~50K entries, performing type checks
and calling `bulkApply` (which checks empty vector sizes) per iteration.

At ~30ns per iteration (two type checks + iterator advance + two size checks),
50K iterations ≈ 1.5ms.

## Trigger

Run `scripts/run_apply_load_matrix.py` with any Soroban scenario. Profile
`LedgerTxnRoot::Impl::commitChild`. The entry iteration time should be
visible but small relative to the SOCI commit.

## Target Code

- `src/ledger/LedgerTxn.cpp:commitChild:2918-2991` — iterates all entries via `EntryIterator`
- `src/ledger/LedgerTxn.cpp:accumulate:2877-2895` — per-entry type check, returns false for non-offer entries
- `src/ledger/LedgerTxn.cpp:bulkApply:2898-2915` — per-iteration check of empty vectors

## Evidence

- The loop at 2942-2953 is unconditional — no short-circuit for zero SQL-eligible entries
- `accumulate` returns false for all Soroban entry types (CONTRACT_DATA, CONTRACT_CODE, TTL)
- `accumulate` also returns false for ACCOUNT (since `LiveBucketIndex::typeNotSupported(ACCOUNT)` is false in recent protocols)
- Only OFFERs are SQL-eligible, and Soroban workloads don't modify offers

## Anti-Evidence

- The iteration cost is only ~1.5ms for 50K entries — well below the 5% threshold for Low severity
- The SOCI commit at line 2959 (`mTransaction->commit()`) still runs regardless (needed for HAS + header SQL writes), so the total function time is dominated by SQL commit latency, not entry iteration
- Adding a "has offers" tracking flag to LedgerTxn adds complexity for minimal gain
- The `EntryIterator` provides lazy iteration over the sealed entry map — the overhead is just pointer chasing + type checks, which is cache-friendly

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The ~1.5ms savings from skipping the entry iteration is dwarfed by the
unavoidable SOCI SQL COMMIT that must still execute in the same function
(for HAS + ledger header writes). The iteration is a trivial fraction (<1%)
of total ledger close time. Even in the most favorable T=8 scenario where
the serial portion matters most, 1.5ms out of ~30-60ms total is only 2.5-5%
— at the threshold of noise in benchmark measurements.

### Lesson Learned

For SQL commit paths, the actual SQL transaction overhead (COMMIT statement
+ journal sync) typically dominates over in-memory iteration of the entry
set. Optimizing the iteration without addressing the SQL overhead provides
negligible benefit.
