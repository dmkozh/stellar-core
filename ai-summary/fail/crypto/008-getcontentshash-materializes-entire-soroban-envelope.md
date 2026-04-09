# H004: `getContentsHash` Materializes The Full Soroban Preimage

**Date**: 2026-04-08
**Subsystem**: crypto
**Severity**: Medium
**Impact**: CPU and allocation overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Computing a transaction's contents hash should stream XDR bytes directly into SHA-256, the same way `xdrSha256` streams full-object hashing without an intermediate buffer. Signed Soroban transactions should not need to allocate and copy a complete `(networkID, envelopeType, tx)` preimage just to obtain the hash used by signature checking.

## Mechanism

`TransactionFrame::getContentsHash` calls `sha256(xdr::xdr_to_opaque(...))`, which first serializes the entire preimage into a temporary vector and then hashes that vector. Soroban envelopes in the apply-load benchmark carry large footprints, auth entries, and resource sections, so every transaction pays a full encode-to-buffer plus hash pass before any signature verification can begin.

## Trigger

Run the default `custom_token` or `soroswap` apply-load scenarios, where Soroban envelopes are larger than minimal SAC transfers. Compare allocator and memcpy samples in `TransactionFrame::getContentsHash` against a build that hashes the XDR stream directly.

## Target Code

- `src/transactions/TransactionFrame.cpp:132-154` - `getContentsHash` serializes then hashes
- `src/transactions/TransactionFrame.cpp:1904-1906` - `checkValid` constructs `SignatureChecker` from the contents hash
- `src/transactions/TransactionFrame.cpp:2066-2067` - apply path constructs `SignatureChecker` from the contents hash
- `src/transactions/TransactionFrame.cpp:2548-2549` - apply entry point always requests the contents hash
- `src/crypto/XDRHasher.h:13-104` - existing zero-copy XDR hashing infrastructure

## Evidence

The code already uses `xdrSha256(mEnvelope)` for `getFullHash`, which shows the project has an allocation-free XDR hashing pattern available. `getContentsHash` is the outlier: it materializes the whole preimage despite being executed once for every benchmark transaction.

## Anti-Evidence

`mContentsHash` is cached on the `TransactionFrame`, so repeated call sites within the same transaction do not multiply the cost. The savings therefore depend on the total number and size of distinct transactions rather than on repeated hashing within one tx.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full code path from `TransactionFrame::getContentsHash()` through `xdr::xdr_to_opaque()` and `sha256()`. Confirmed that `xdr_to_opaque` performs two full passes over the XDR tree: first `xdr_argpack_size()` to compute the buffer size, then `xdr_argpack_archive()` to serialize into a heap-allocated `opaque_vec<>`. The streaming alternative (`XDRSHA256` via `XDRHasher`) is already used by `TransactionFrame::getFullHash()` and would eliminate the size-computation traversal, the heap allocation, and the intermediate copy. The fix is mechanically simple because `xdr::xdr_argpack_archive` already accepts any xdrpp-compatible archiver with variadic arguments.

### Code Paths Examined

- `src/transactions/TransactionFrame.cpp:132-158` — `getContentsHash()` calls `sha256(xdr::xdr_to_opaque(mNetworkID, ENVELOPE_TYPE_TX, mEnvelope.v1().tx))` for V1 envelopes and similar for V0. Result cached in `mContentsHash`.
- `src/transactions/FeeBumpTransactionFrame.cpp:584-593` — `getContentsHash()` uses the same `sha256(xdr::xdr_to_opaque(...))` pattern for fee-bump transactions.
- `src/transactions/FeeBumpTransactionFrame.cpp:595-603` — `getFullHash()` also uses `sha256(xdr::xdr_to_opaque(mEnvelope))` instead of `xdrSha256(mEnvelope)`, an additional inconsistency with `TransactionFrame::getFullHash()`.
- `lib/xdrpp/xdrpp/marshal.h:264-272` — `xdr_to_opaque` computes size via `xdr_argpack_size`, allocates `opaque_vec<>`, serializes via `xdr_argpack_archive`.
- `lib/xdrpp/xdrpp/marshal.h:237-246` — `xdr_argpack_archive` is variadic and works with any archiver, including `XDRSHA256`.
- `src/crypto/SHA.h:36-61` — `XDRSHA256` struct and single-arg `xdrSha256` template. Only takes one argument; needs variadic extension.
- `src/crypto/XDRHasher.h:16-104` — `XDRHasher` CRTP base with 256-byte internal buffer, avoiding per-field hash calls.
- `src/transactions/TransactionFrame.cpp:125-129` — `getFullHash()` correctly uses `xdrSha256(mEnvelope)` (streaming, no allocation).

### Findings

The inefficiency is **real**: `getContentsHash` performs an unnecessary XDR size-computation traversal, heap allocation, serialization into buffer, then hashing of that buffer — when the streaming `XDRSHA256` archiver could hash in a single pass with only a 256-byte stack buffer.

However, severity is **Informational** rather than Medium because:

1. **Cached once per transaction**: `mContentsHash` is computed once and reused across all call sites within the same transaction lifecycle. The overhead is per-distinct-transaction, not per-call.
2. **Small fraction of total tx cost**: For a typical 2-4KB Soroban transaction, the `getContentsHash` overhead (size traversal ~300ns + malloc ~100ns + free ~100ns ≈ 500ns) is <1% of total per-transaction processing time (100-500μs including Soroban host invocation).
3. **SHA-256 dominates**: The SHA-256 computation itself (~1-2μs for 2-4KB) is the majority of `getContentsHash` time. The streaming approach does the same amount of hashing; it only eliminates the size computation and allocation overhead.

The optimization saves roughly 15-30% of `getContentsHash`'s own cost, but since `getContentsHash` is ~0.5-2% of total benchmark time, the net benchmark improvement is ~0.1-0.5% — below the measurable threshold for any benchmark scenario.

Additionally, `FeeBumpTransactionFrame::getFullHash()` at line 600 uses `sha256(xdr::xdr_to_opaque(mEnvelope))` instead of `xdrSha256(mEnvelope)`, which is a pure consistency bug that should be fixed as part of this change.

### PoC Guidance

- **Target code**:
  - `src/crypto/SHA.h` — make `xdrSha256` variadic: replace the single-arg template with `template <typename... Args> uint256 xdrSha256(Args const&... args)` using `xdr::xdr_argpack_archive` instead of `xdr::archive`
  - `src/transactions/TransactionFrame.cpp:146-152` — replace `sha256(xdr::xdr_to_opaque(...))` with `xdrSha256(...)` in both V0 and V1 branches
  - `src/transactions/FeeBumpTransactionFrame.cpp:589` — replace `sha256(xdr::xdr_to_opaque(...))` with `xdrSha256(...)` in `getContentsHash`
  - `src/transactions/FeeBumpTransactionFrame.cpp:600` — replace `sha256(xdr::xdr_to_opaque(mEnvelope))` with `xdrSha256(mEnvelope)` in `getFullHash`
- **Change description**: Make `xdrSha256` variadic to support multi-arg hashing, then replace all `sha256(xdr::xdr_to_opaque(...))` call sites in transaction frame classes with the streaming equivalent. This eliminates per-transaction heap allocation and an extra XDR tree traversal.
- **Correctness check**: `xdr_argpack_archive` calls `xdr::archive` for each argument in sequence, producing identical byte streams. Existing tests in `src/crypto/test/CryptoTests.cpp` verify `xdrSha256` matches `sha256(xdr_to_opaque(...))` for single args. The PoC should add a multi-arg equivalence test. All transaction hash-dependent tests (signature verification, tx set hashing) serve as regression tests.
- **Benchmark focus**: Measure allocator overhead in `getContentsHash` (heap allocation count should drop to zero). Overall benchmark improvement expected to be <1%, so focus on per-function profiling rather than end-to-end throughput.

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-08
**PoC by**: claude-opus-4.6, high

### Changes Made

- **`src/crypto/SHA.h:53-61`** — Made `xdrSha256` variadic: changed `template <typename T>` to `template <typename... Args>` and replaced `xdr::archive(xs, t)` with `xdr::xdr_argpack_archive(xs, args...)`. This enables streaming multi-argument XDR hashing without intermediate buffer allocation.

- **`src/transactions/TransactionFrame.cpp:146-152`** — Replaced `sha256(xdr::xdr_to_opaque(mNetworkID, ENVELOPE_TYPE_TX, 0, mEnvelope.v0().tx))` and `sha256(xdr::xdr_to_opaque(mNetworkID, ENVELOPE_TYPE_TX, mEnvelope.v1().tx))` with equivalent `xdrSha256(...)` calls in both V0 and V1 branches of `getContentsHash()`.

- **`src/transactions/FeeBumpTransactionFrame.cpp:589`** — Replaced `sha256(xdr::xdr_to_opaque(mNetworkID, ENVELOPE_TYPE_TX_FEE_BUMP, mEnvelope.feeBump().tx))` with `xdrSha256(...)` in `getContentsHash()`.

- **`src/transactions/FeeBumpTransactionFrame.cpp:600`** — Replaced `sha256(xdr::xdr_to_opaque(mEnvelope))` with `xdrSha256(mEnvelope)` in `getFullHash()`, fixing the consistency bug where this method used the allocating pattern while `TransactionFrame::getFullHash()` already used streaming.

### Demonstration

The optimization eliminates per-transaction heap allocation and an extra XDR tree traversal pass in `getContentsHash()` for all transaction types (V0, V1, and fee-bump). By making `xdrSha256` variadic and using `xdr::xdr_argpack_archive`, the XDR preimage bytes are now streamed directly into SHA-256 through the existing 256-byte stack buffer in `XDRHasher`, avoiding the `xdr_argpack_size` + `malloc` + `xdr_argpack_archive` + `sha256` + `free` sequence that `xdr_to_opaque` required. This also fixes the inconsistency in `FeeBumpTransactionFrame::getFullHash()`.

### Test Results

- All 16 crypto tests passed (15,262 assertions)
- All 124 transaction tests passed (558,956 assertions)
- All 7 fee bump tests passed (1,402 assertions)
- Full test suite (`make check`) passed — all tests passed with no regressions

---

## Final Review

**Verdict**: REJECTED
**Date**: 2026-04-09
**Final review by**: gpt-5.4, high
**Failed At**: final-review

### Adversarial Analysis

1. **Exercises claimed inefficiency**: PASS — `TransactionFrame::getContentsHash()` and `FeeBumpTransactionFrame::getContentsHash()` were switched from `sha256(xdr::xdr_to_opaque(...))` to streaming `xdrSha256(...)`, which removes the temporary `opaque_vec` allocation and the extra `xdr_argpack_size` traversal the hypothesis identified.
2. **Realistic preconditions**: PASS — apply-load signs and validates distinct Soroban envelopes on every transaction, so these code paths are exercised under the `custom_token` and `soroswap` scenarios the hypothesis targeted.
3. **Inefficiency vs by-design**: PASS — the removed materialization is not required for correctness; the streaming form is semantically equivalent and already used elsewhere, including `TransactionFrame::getFullHash()`.
4. **Benchmark impact / severity**: FAIL — the benchmark signal is not stable enough to support a finding. Against `ai-summary/baseline.csv`, the first optimized run (`/home/devbox/apply-load/finalreview-getcontentshash-20260409-001853/results.csv`) showed wins in `soroswap` and mixed `custom_token`, but the rerun (`/home/devbox/apply-load/finalreview-getcontentshash-rerun-20260409-004538/results.csv`) flipped several key rows: `custom_token,TX=3000,T=1` moved from **-8.32% / -14.29% / -19.84%** (median/p95/p99) to **+5.27% / +4.27% / +4.83%**; `custom_token,TX=3000,T=8` moved from **+4.32% / +5.76% / +4.14%** to **-1.15% / +0.96% / -1.86%**; and `soroswap,TX=1600,T=8` moved from **+5.32% / +5.56% / +5.44%** to **+0.75% / +0.49% / -1.23%**.
5. **In scope**: PASS — this is a C++ apply-path optimization in the crypto/transaction hashing boundary and does not touch soroban-env-host internals.
6. **Benchmark methodology**: PASS — stellar-core was rebuilt, the full existing test suite completed successfully, and the project benchmark tool was run twice exactly as required: `python3 scripts/run_apply_load_matrix.py --stellar-core-bin ./src/stellar-core --build-tag finalreview-getcontentshash` and `python3 scripts/run_apply_load_matrix.py --stellar-core-bin ./src/stellar-core --build-tag finalreview-getcontentshash-rerun`.
7. **Alternative explanations / attribution**: FAIL — the same binary produced materially different outcomes for the same scenarios across two back-to-back runs, including sign flips between regression and improvement. That level of instability means the apparent wins can be explained by ordinary benchmark variance or environment noise rather than the hashing change itself.
8. **Novelty**: PASS — no duplicate of this exact finding was identified in the existing crypto fail set.

### Rejection Reason

The code change is semantically safe and does remove the temporary-buffer work the hypothesis described, but the authoritative apply-load benchmark does not produce a reproducible improvement. Because the same scenarios swing between regressions and gains across two independent runs, the performance claim is not attributable enough to confirm.

### Failed Checks

- 4
- 7
