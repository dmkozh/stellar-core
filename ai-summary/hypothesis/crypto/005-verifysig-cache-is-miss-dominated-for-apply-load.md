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
