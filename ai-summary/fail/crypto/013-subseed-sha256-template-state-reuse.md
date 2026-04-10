# H013: Template SHA-256 State Reuse for subSha256 PRNG Seed Derivation

**Date**: 2026-04-10
**Subsystem**: crypto
**Severity**: Informational
**Impact**: reduced per-transaction SHA-256 init overhead
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When deriving per-transaction PRNG sub-seeds via `subSha256(sorobanBasePrngSeed, counter)`, the system should avoid re-initializing SHA-256 state and re-hashing the same 32-byte seed for every transaction. Since `sorobanBasePrngSeed` is constant across all transactions in a ledger, the SHA-256 state after absorbing the seed could be pre-computed once and cloned per transaction, saving one `crypto_hash_sha256_init` + one `crypto_hash_sha256_update` per transaction.

## Mechanism

`subSha256` (SHA.cpp:30-38) creates a new `SHA256` object for each call, which calls `crypto_hash_sha256_init` (~20ns). It then calls `sha.add(seed)` to update with 32 bytes of `sorobanBasePrngSeed` (~30ns), followed by `sha.add(counter)` with 8 bytes (~20ns), and `sha.finish()` which pads to 64 bytes and compresses one SHA-256 block (~200ns). Total: ~270ns per call.

With a pre-computed template state (SHA-256 state after absorbing the 32-byte seed), each transaction would only need: memcpy of 104-byte state (~10ns) + update 8-byte counter (~20ns) + finalize (~200ns) = ~230ns. Savings: ~40ns per transaction.

In `applyThread` (LedgerManagerImpl.cpp:2396), `subSha256` is called once per Soroban transaction. For 3200 SAC transactions, total savings = ~40ns × 3200 = ~0.13ms per ledger close.

## Trigger

Run SAC apply-load at TX=6400. Measure cumulative time in `subSha256` during parallel apply. Compare against a build that pre-computes the SHA-256 state template and clones it per transaction.

## Target Code

- `src/crypto/SHA.cpp:30-38` — subSha256 creates new SHA256 per call
- `src/crypto/SHA.cpp:41-53` — SHA256 constructor calls crypto_hash_sha256_init
- `src/ledger/LedgerManagerImpl.cpp:2396` — applyThread calls subSha256 per tx
- `src/ledger/LedgerManagerImpl.cpp:2808` — sequential apply calls subSha256 per tx
- `src/transactions/TransactionFrame.cpp:2574` — per-operation subSha256 in apply

## Evidence

The `sorobanBasePrngSeed` is derived once from `txSet.getContentsHash()` (LedgerManagerImpl.cpp:2639) and passed unchanged to every `applyThread` call. The first 32 bytes of SHA-256 input are identical across all transactions in the ledger. SHA-256's incremental API supports state cloning via simple struct copy of `crypto_hash_sha256_state` (104 bytes).

## Anti-Evidence

The total savings is ~0.13ms per ledger close, approximately 0.01-0.1% of benchmark time. This is far below the measurement noise floor. The `subSha256` function was already optimized in a previous commit to avoid `xdr::xdr_to_opaque` heap allocation for the counter (SHA.cpp:36-37, using `xdr::swap64le` directly on the stack). The remaining overhead is dominated by the SHA-256 compression function (~200ns), which cannot be avoided regardless of state reuse.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The savings of ~40ns per transaction × ~3200 transactions = ~0.13ms is approximately 0.01% of a typical apply-load ledger close time (~100-500ms). This is three orders of magnitude below the minimum severity threshold (5% improvement). The SHA-256 compression function dominates `subSha256` cost, and state initialization is a negligible fraction. No benchmark scenario would show measurable improvement from this optimization.

### Lesson Learned

SHA-256 init (`crypto_hash_sha256_init`) and small-input update costs are negligible (~20-30ns each) compared to the compression function (~200ns). Optimizing the init/update pattern for `subSha256` produces savings that are invisible at the benchmark level. Per-transaction crypto overhead in the apply path is dominated by hash compression, not by state management or function call dispatch.
