# H020: Skip Apply-Time Signature Verification After Tx-Set Validation

**Date**: 2026-04-10
**Subsystem**: crypto
**Severity**: Low
**Impact**: redundant signature work on the measured close path
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once a transaction has already passed tx-set validation, the measured apply path
should ideally avoid re-running the same signature verification work unless the
later phase needs some additional state that was not captured during validation.

## Mechanism

The benchmark validates transactions before apply, and then `commonPreApply`
constructs another `SignatureChecker` and runs `commonValid` again during the
measured close. If that third signature pass were semantically unnecessary, the
apply path could skip it and save the remaining signature-cache-hit work.

## Trigger

Run the high-transaction SAC benchmark and profile the apply phase around
`commonPreApply` / `checkAllTransactionSignatures`, then compare against a build
that tries to reuse earlier validation results instead of re-verifying during
apply.

## Target Code

- `src/herder/TxSetUtils.cpp:187-191` — first tx-set validation pass calls `tx->checkValid(...)`
- `src/herder/TxSetFrame.cpp:1786-1793` — tx-set validation can run that pass again when `txsAreValidated` is false
- `src/transactions/TransactionFrame.cpp:1730-1735` — `commonValid` performs transaction signature checking
- `src/transactions/TransactionFrame.cpp:2049-2117` — `commonPreApply` rebuilds `SignatureChecker`, reruns `commonValid`, then processes signer side effects
- `src/transactions/TransactionFrame.cpp:2148-2166` — `preParallelApply` invokes `commonPreApply` for every Soroban tx before measured parallel apply

## Evidence

The third pass is real and happens on the benchmarked path. The benchmark also
does substantial validation before apply, so at first glance it looks like a
candidate for reuse.

## Anti-Evidence

`commonPreApply` is not just a pure “did the signatures verify?” check: it also
flows through `processSignatures` and the surrounding pre-apply bookkeeping that
feeds signer-consumption semantics and account updates. Reusing earlier results
would require carrying forward exactly which signatures were consumed and all
relevant signer-side effects, not just a boolean “auth passed” bit.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

By the time the measured apply path reaches `commonPreApply`, the earlier tx-set
validation passes have already warmed the signature cache, so the remaining work
is the cheap cache-hit path rather than raw Ed25519 verification. Eliminating
the third pass would therefore save only cache-hit bookkeeping, while the
correctness plumbing needed to preserve signer-consumption semantics spans much
more than a simple skip flag.

### Lesson Learned

For apply-load, signature optimizations must target either uncached
verification *inside* the measured window or a large amount of non-crypto
side-work around signature checking. Once the cache is warm, simply removing one
verification phase is ceiling-bounded by cache-hit cost.
