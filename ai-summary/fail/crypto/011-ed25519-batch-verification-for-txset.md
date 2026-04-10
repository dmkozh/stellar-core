# H011: Ed25519 Batch Verification Could Accelerate Signature Validation

**Date**: 2026-04-10
**Subsystem**: crypto
**Severity**: High
**Impact**: 2-3x faster signature verification for uncached signatures
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When validating a tx set containing N transactions, each with one Ed25519 signature, the system should verify all N signatures as efficiently as possible. Ed25519 batch verification (via multi-scalar multiplication) can verify N signatures in roughly O(N) time with a constant factor 2-3x smaller than N individual verifications, saving 50-75% of the CPU time spent on signature verification.

## Mechanism

The `ed25519-dalek` crate (v2.1.1, already linked) declares a `batch` feature that enables `verify_batch()` — a function that verifies multiple signatures simultaneously using Schnorr-style batch verification with random linear combination. The crate is linked with features `[alloc, default, fast, rand_core, std, zeroize]` but `batch` is NOT enabled (see fingerprint at `src/rust/soroban/p24/target/release/.fingerprint/ed25519-dalek-*/lib-ed25519_dalek.json`).

Currently, `PubKeyUtils::verifySig` verifies signatures one-by-one (SecretKey.cpp:479-488), either through libsodium (`crypto_sign_verify_detached`) or Rust dalek (`verify_ed25519_signature_dalek`). For a ledger with 3200 SAC transactions, each with 1 signature, the first validation pass produces ~3200 cache misses requiring individual Ed25519 verifications at ~100μs each = ~320ms total. Batch verification could reduce this to ~100-160ms.

## Trigger

Run the SAC apply-load scenario at TX=6400 (which generates the most signatures). Measure total time in Ed25519 verification during the first `checkValid` pass. Compare against a build that collects all uncached signatures and verifies them in a single `verify_batch` call.

## Target Code

- `src/crypto/SecretKey.cpp:447-495` — verifySig verifies signatures individually
- `src/rust/src/ed25519_verify.rs` — current per-signature Rust verify function
- `src/rust/src/bridge.rs` — FFI bridge (would need new batch verify function)
- `src/rust/Cargo.toml` — ed25519-dalek dependency (batch feature not enabled)
- `src/simulation/ApplyLoad.cpp:2138-2149` — benchmark pre-caches all signatures

## Evidence

Ed25519-dalek v2.1.1 declares the `batch` feature with `verify_batch` support. The crate is already linked. Enabling `batch` would add the `merlin` dependency (transcript-based batching). For 1000+ signatures, batch verification achieves 2-3x speedup over individual verification due to multi-scalar multiplication optimizations.

## Anti-Evidence

The apply-load benchmark explicitly pre-warms the signature verification cache before measurement begins (ApplyLoad.cpp:2138-2149, comment: "prime the signature cache ... excluding the verification from the benchmark is likely more realistic than including it"). This means during the measured benchmark time, ALL signature verifications hit the cache — no actual Ed25519 verification occurs. The batch verification optimization would have ZERO impact on the measured apply-load benchmark.

Furthermore, in the parallel apply path (T=8), `TransactionFrame::parallelApply` does not perform signature verification at all — it trusts that signatures were validated during tx set construction. So even if the cache were not pre-warmed, batch verification would only help the pre-apply validation phase, not the parallel apply phase.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The apply-load benchmark deliberately pre-warms the signature verification cache before the measured benchmark begins (ApplyLoad.cpp:2138-2149). During the measured ledger close time, all signature verifications are cache hits (~0.5μs each, no Ed25519 computation). The batch verification optimization would save ~160-240ms of Ed25519 computation, but this computation occurs during tx generation/validation OUTSIDE the measured time window. Additionally, Soroban transactions in the parallel apply path skip signature verification entirely. The optimization has genuine value for production workloads (where the first verification pass IS on the critical path), but it cannot improve the apply-load benchmark.

### Lesson Learned

The apply-load benchmark separates tx generation/validation (including signature cache warming) from the measured ledger close. Crypto optimizations that target the first-verification-pass (cache misses) will show zero impact on the benchmark. Only optimizations that affect the cache-hit path or non-signature crypto operations within the measured window can potentially improve benchmark results.
