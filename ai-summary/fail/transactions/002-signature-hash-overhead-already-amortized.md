# H002: Signature and contents-hash overhead is already amortized

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Low
**Impact**: signature verification / hash computation
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The benchmark should not pay large cold costs for transaction hash computation or signature verification during ledger close, because generated transactions are prevalidated before the timer starts and transaction contents hashes are memoized on the frame object. Any remaining cost in pre-apply should come from broader transaction setup, not repeated hash recomputation or empty-cache signature checks.

## Mechanism

I investigated whether `commonPreApply` was dominated by `getContentsHash()` and signature verification. The code already avoids the narrow failure mode: `TransactionFrame::getContentsHash()` caches `mContentsHash`, and apply-load explicitly calls `checkValid()` on every generated tx to prime the signature cache before benchmarking. That leaves some residual signature-processing work inside `commonPreApply`, but not the specific "cold hash / cold signature cache" issue.

## Trigger

Generate benchmark transactions via `ApplyLoad::generateSacPayments`, `generateTokenTransfers`, or `generateSoroswapSwaps`, then close a benchmark ledger with the default template config.

## Target Code

- `src/transactions/TransactionFrame.cpp:TransactionFrame::getContentsHash:133-158` - memoizes the transaction contents hash
- `src/transactions/TransactionFrame.cpp:TransactionFrame::commonPreApply:2048-2080` - still builds `SignatureChecker`, but uses cached contents hash
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSacPayments:2138-2148` - explicitly primes the signature cache
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateTokenTransfers:2336-2341` - prevalidates generated txs
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSoroswapSwaps:3201-3206` - prevalidates generated txs

## Evidence

The benchmark generator comment says the up-front validation pass is there to "prime the signature cache". `TransactionFrame::getContentsHash()` stores the hash in `mContentsHash` after the first computation, so later `SignatureChecker` construction reuses the cached hash instead of re-encoding the envelope contents.

## Anti-Evidence

`commonPreApply` still performs more than just cache lookups: it processes sequence numbers, signatures, resource fees, and `commonValid`. Those broader serial costs remain plausible optimization targets.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The suspected narrow bottleneck is already mitigated by hash memoization plus explicit signature-cache priming during benchmark tx generation.

### Lesson Learned

When a pre-apply phase still looks expensive after cache priming, target the remaining serial validation/state-bookkeeping around it rather than the raw signature/hash primitives.
