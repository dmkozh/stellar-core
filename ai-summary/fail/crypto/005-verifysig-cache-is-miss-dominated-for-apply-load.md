# H005: Global Signature Cache Is Net-Negative For Unique Apply-Load Txs

**Date**: 2026-04-08
**Subsystem**: crypto
**Severity**: Low
**Impact**: wasted hashing and cache traffic
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Transaction signature verification on the check-valid/apply path should avoid global-cache work when the workload consists of one-shot signatures with effectively zero reuse. In that case, the fast path should go straight to Ed25519 verification instead of first building a cache key, probing a process-wide cache, and inserting a miss that will never be reused.

## Mechanism

`PubKeyUtils::verifySig` always computes a BLAKE2 cache key from `(public key, signature, message)` and always touches the global cache around that key. Because the message component is the transaction contents hash, and apply-load transactions necessarily change sequence numbers and Soroban payloads across envelopes, the benchmark should be overwhelmingly miss-dominated; the cache then adds BLAKE2 work plus two lock acquisitions around almost every verification.

## Trigger

Run any default apply-load scenario and inspect signature-cache hit rate alongside CPU samples in `verifySigCacheKey` and `PubKeyUtils::verifySig`. Compare against a build that bypasses the global cache for check-valid/apply or that disables the cache once miss-rate crosses a threshold for the current workload.

## Target Code

- `src/crypto/SecretKey.cpp:55-66` - `verifySigCacheKey` hashes key, signature, and message with BLAKE2
- `src/crypto/SecretKey.cpp:447-495` - `PubKeyUtils::verifySig` always probes and populates the global cache
- `src/transactions/SignatureChecker.cpp:117-135` - tx validation funnels Ed25519 verification through this path
- `src/transactions/TransactionFrame.cpp:1904-1906` - `checkValid` uses contents-hash-based signature checking
- `src/transactions/TransactionFrame.cpp:2066-2067` - apply path uses the same contents-hash-based signature checking

## Evidence

The cache key explicitly includes the message bytes, so any change to the transaction contents produces a distinct lookup key. The benchmark scenarios generate large volumes of signed Soroban transactions, making it plausible that almost every lookup is a cold miss rather than a reused verification result.

## Anti-Evidence

The global cache is valuable in other workloads, especially when the same envelope is re-verified by multiple subsystems or background validation warms the cache. Any optimization here likely needs to be scoped to the apply/check-valid path or guarded by observed hit rate rather than removing the cache universally.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Each transaction in the ApplyLoad benchmark goes through signature verification **three times**, not once: (1) during `makeTxSetFromTransactions` → `trimInvalid` → `getInvalidTxListWithErrors` → `tx->checkValid()`, (2) during `txSet.second->checkValid()` → `checkValidInternalWithResult(..., txsAreValidated=false)` → `getInvalidTxListWithErrors` → `tx->checkValid()` again, and (3) during `apply` → `commonPreApply` → `commonValid` → `checkAllTransactionSignatures`. Only the first pass is a cache miss; the second and third are cache hits. The cache is therefore strongly net-positive (~66% hit rate), saving 2 out of 3 Ed25519 verifications per transaction.

### Code Paths Examined

- `src/crypto/SecretKey.cpp:55-66` — `verifySigCacheKey` computes BLAKE2 over (pubkey ∥ sig ∥ message), ~128 bytes total. This takes ~1μs, <1% of Ed25519 verify time (~100-200μs).
- `src/crypto/SecretKey.cpp:447-495` — `PubKeyUtils::verifySig` acquires mutex, checks cache, verifies on miss, inserts result. On hit, returns immediately after one lock+lookup.
- `src/transactions/SignatureUtils.cpp:38-46` — `SignatureUtils::verify` calls `PubKeyUtils::verifySig` with the transaction's `contentsHash` as the message, which is the same across all verification passes for a given tx.
- `src/herder/TxSetUtils.cpp:187-210` — `getInvalidTxListWithErrors` calls `tx->checkValid()` on every tx (first verification pass, all misses).
- `src/herder/TxSetFrame.cpp:1786-1793` — `TxSetPhaseFrame::checkValidWithResult` with `txsAreValidated=false` calls `getInvalidTxListWithErrors` again (second pass, all hits).
- `src/test/TxTests.cpp:637` — `txSet.second->checkValid(app, 0, 0)` triggers the second pass above.
- `src/transactions/TransactionFrame.cpp:2066-2067,2091` — `commonPreApply` → `commonValid` does the third verification (all hits).
- `src/overlay/Peer.cpp:80-93` — Background signature validation also warms the cache in production (not benchmarks), providing additional hits.

### Why It Failed

The hypothesis's core claim — that the cache is "miss-dominated" and therefore "net-negative" — is based on the incorrect assumption that each unique transaction has its signatures verified only once. In the actual ApplyLoad benchmark flow, each transaction's signatures are verified three times (two during tx set construction/validation, once during apply). The `contentsHash` is identical across all three passes for the same transaction, so the cache key is the same, yielding a ~66% hit rate (1 miss, 2 hits per tx). The cache saves approximately 200-400μs of Ed25519 verification per transaction.

Furthermore, even in a hypothetical 100%-miss scenario, the cache overhead per call is ~1-2μs (BLAKE2 hash of 128 bytes + uncontended mutex), which is <2% of the ~100-200μs Ed25519 verification cost. The cache cannot be "net-negative" because its overhead is noise-level compared to the operation it caches.

### Lesson Learned

When analyzing signature verification cache effectiveness, trace the full lifecycle of a transaction through all validation stages (tx set construction, tx set validation, and apply). The same transaction is verified multiple times in the normal flow, and the cache's primary value is eliminating redundant verification across these stages — not across different transactions. The contentsHash is the same for a given transaction across all stages, making cross-stage cache hits the dominant pattern.
