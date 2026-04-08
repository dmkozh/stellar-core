# H001: Offer Statement Cache Regression Is Off-Benchmark

**Date**: 2026-04-08
**Subsystem**: database
**Severity**: Medium
**Impact**: CPU overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

An apply-load optimization should target database work that the SAC,
custom-token, and soroswap benchmark scenarios actually execute. Reintroducing a
prepared-statement cache would only matter if the benchmark exercised the
hot-loop SQL prepare sites it is meant to save.

## Mechanism

The recent removal of the database prepared-statement cache is an obvious
performance suspect because `Database::getPreparedStatement()` now allocates and
prepares a fresh SOCI statement on every call. But the hot call sites are in
`LedgerTxnOfferSQL.cpp`, and only `OFFER` remains on the SQL backend while the
apply-load benchmark scenarios are Soroban-only.

## Trigger

Run `scripts/run_apply_load_matrix.py` with any of the built-in scenarios. None
of the benchmark model transactions go through the classic order-book / offer
SQL path.

## Target Code

- `src/database/Database.cpp:758-765` — prepared statements are freshly allocated and prepared on every call
- `src/ledger/LedgerTxnOfferSQL.cpp:25-852` — hot offer load/upsert/delete paths still prepare statements repeatedly
- `src/bucket/LiveBucketIndex.cpp:22-25` — only `OFFER` is left outside BucketListDB
- `scripts/run_apply_load_matrix.py:71-101` — benchmark scenarios are `sac`, `custom_token`, and `soroswap`

## Evidence

`git show c8a3f1f87` shows the cache was removed recently, and
`LedgerTxnOfferSQL.cpp` still contains many per-call prepares. If the benchmark
were offer-heavy this would be a strong candidate.

## Anti-Evidence

The benchmark does not touch offer SQL paths, so even a real regression there
would not move the apply-load matrix this objective cares about.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-08
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The only obviously hot prepared-statement sites left in apply are offer-table
queries, and the target benchmark scenarios do not exercise `OFFER` state.

### Lesson Learned

In the database subsystem, "looks hot" is not enough; for apply-load work, first
check whether the path survives the BucketListDB split and is still hit by
Soroban model transactions.
