# H001: Fast Apply-Time Validation for Soroban preParallelApply

**Date**: 2025-07-14
**Subsystem**: transaction-ledger
**Severity**: Medium
**Impact**: 10% reduction in SAC T=8 ledger close time; 5-7% for other scenarios
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `preParallelApply` processes each Soroban transaction, it should perform
only the state-dependent operations that are necessary for correctness during
apply: fee deduction, sequence number advancement, and refundable fee tracker
initialization. Static validation (Soroban resource checks, footprint dedup,
signature verification, time bounds, operation-level checkValid) was already
performed at flood/nomination time and cannot change between validation and
apply for Soroban-only transaction sets.

The expected apply-time cost per tx should be ~4µs (fee computation + LedgerTxn
fee/seq processing + commit), not the current ~14µs that includes redundant
re-validation.

## Mechanism

`preParallelApply` calls `commonPreApply` which runs the full `commonValid` →
`commonValidPreSeqNum` chain plus `OperationFrame::checkValid` for every
Soroban transaction. This redundantly re-validates static properties:

- `checkSorobanResources`: iterates all footprint keys calling `xdr_size` on
  each, plus resource limit comparisons (~1.5µs)
- Footprint dedup: allocates an `UnorderedSet<LedgerKey>`, inserts all ~7
  footprint keys with hashing, then destroys the set (~1µs)
- `checkAllTransactionSignatures`: constructs `SignatureChecker`, computes
  BLAKE2 cache key, takes mutex lock, does cache lookup (~2µs on cache hit)
- `processSignatures`: creates another `LedgerSnapshot`, calls
  `checkOperationSignatures`, `removeOneTimeSignerFromAllSourceAccounts`,
  `checkAllSignaturesUsed` (~2µs)
- `OperationFrame::checkValid`: loads source account from snapshot again,
  calls `doCheckValidForSoroban` (~1.1µs)
- Protocol version checks, time bounds, fee sufficiency, memo validation,
  `LedgerSnapshot` construction for validation (~2µs)

None of these checks can fail for a Soroban transaction that passed flood-time
validation, because: (1) static resource/footprint constraints don't change
within a ledger, (2) signatures can't be invalidated by other Soroban txs in
the set (no classic ops to remove signers), (3) time bounds are checked against
the same ledger close time. The total redundant work is ~9.75µs per tx.

For 6400 SAC transactions at T=8: 6400 × 9.75µs = **62.4ms** of serial
redundant validation, which is **10.2%** of the 612ms p50 close time.

## Trigger

Run apply-load benchmark with SAC scenario at TX=6400, T=8. The
`preParallelApply` loop in `GlobalParallelApplyLedgerState::
preParallelApplyAndCollectModifiedClassicEntries` processes all 6400 txs
serially. Profile with Tracy or perf to measure time spent in
`commonPreApply` → `commonValid` → `commonValidPreSeqNum` and
`OperationFrame::checkValid`.

## Target Code

- `src/transactions/TransactionFrame.cpp:commonPreApply:2049-2123` — the
  full pre-apply function that bundles validation with fee/seq processing
- `src/transactions/TransactionFrame.cpp:preParallelApply:2126-2188` — calls
  commonPreApply then OperationFrame::checkValid
- `src/transactions/TransactionFrame.cpp:commonValid:1666-1774` — full
  re-validation including static checks
- `src/transactions/TransactionFrame.cpp:commonValidPreSeqNum:1319-1490` —
  static validation including checkSorobanResources and footprint dedup
- `src/transactions/TransactionFrame.cpp:processSignatures:1584-1636` —
  signature processing including checkOperationSignatures
- `src/transactions/OperationFrame.cpp:checkValid:282-359` — operation
  re-validation including doCheckValidForSoroban
- `src/transactions/ParallelApplyUtils.cpp:363-372` — serial loop calling
  preParallelApply on all txs

## Evidence

1. **Fails 016 and 017** identified signature re-verification (~2µs) and
   resource re-validation (<1µs) as individually too small. But the COMBINED
   cost of ALL redundant validation components is ~9.75µs per tx — 5x larger
   than any individual component.

2. The code comment at TransactionFrame.cpp:2162 explicitly acknowledges that
   `checkValid` was moved TO the serial phase FROM the parallel phase:
   "Pre parallel soroban, OperationFrame::checkValid is called right before
   OperationFrame::doApply, but we do it here instead to avoid making
   OperationFrame::checkValid thread safe." This confirms it was a deliberate
   tradeoff, not a correctness requirement.

3. `checkSorobanResources` calls `xdr::xdr_size(key)` on every footprint key
   (line 951) and `this->getSize()` on the full envelope (line 979-980) —
   traversing XDR structures redundantly.

4. Footprint dedup creates and destroys a heap-allocated `UnorderedSet` with
   ~7 entries per tx — 6400 allocations per ledger (line 1464-1489).

5. `SignatureChecker` construction allocates a `unique_ptr` and resizes a
   vector per tx (line 2071-2072). With cache hits, the crypto is fast but
   the BLAKE2 hash computation + mutex + cache lookup still cost ~2µs.

## Anti-Evidence

1. **Defensive validation is important for correctness.** In theory, a tx
   could become invalid between flood validation and apply if ledger state
   changes unexpectedly. However, for Soroban parallel apply: (a) fee
   sufficiency is the only state-dependent check, and it's handled by
   `processFeeSeqNum`-equivalent code in `commonPreApply`; (b) no classic ops
   in the parallel set can modify signers or sequence parameters.

2. **The fast path would need careful protocol gating.** Skipping validation
   changes the failure mode: a previously-detectable bad tx would silently
   proceed to the parallel phase and fail there (or worse, succeed with
   incorrect state). The optimization must ensure the parallel phase handles
   validation-failed txs gracefully (it already does — `parallelApply` checks
   `txResult.isSuccess()` before proceeding).

3. **Code complexity.** Adding a separate `fastPreParallelApply` path
   increases maintenance burden. The existing `commonPreApply` is shared
   between parallel and sequential apply paths.
