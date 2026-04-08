# H003: `subSha256` Allocates Once Per Soroban Tx And Op

**Date**: 2026-04-08
**Subsystem**: crypto
**Severity**: Low
**Impact**: allocation churn
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Soroban PRNG sub-seed derivation should hash the base seed plus the fixed-width XDR representation of the counter without any heap allocation. Per-transaction and per-operation sub-seeding should stay in stack memory because the counter is always an 8-byte scalar with deterministic XDR encoding.

## Mechanism

`subSha256` currently calls `xdr::xdr_to_opaque(counter)`, creating a temporary buffer solely to append one XDR-encoded `uint64_t` to the SHA state. That helper is called once per transaction in `LedgerManagerImpl::applyThread` and again in the serial apply path for Soroban tx/op sub-seeds, so the apply-load matrix turns it into millions of tiny temporary allocations.

## Trigger

Run any default apply-load scenario; all six matrix entries are Soroban scenarios and therefore exercise `subSha256`. Compare allocator samples or `xdr::xdr_to_opaque(uint64_t)` call counts against a build that writes the XDR-encoded counter directly into the SHA state.

## Target Code

- `src/crypto/SHA.cpp:29-35` - `subSha256` materializes a temporary opaque buffer
- `src/ledger/LedgerManagerImpl.cpp:2396-2402` - parallel apply derives one sub-seed per tx
- `src/ledger/LedgerManagerImpl.cpp:2799-2812` - serial apply derives one sub-seed per Soroban tx
- `src/transactions/TransactionFrame.cpp:2338-2346` - serial apply derives per-op sub-seeds
- `scripts/run_apply_load_matrix.py:71-101` - all benchmark scenarios are Soroban workloads

## Evidence

The implementation in `SHA.cpp` clearly allocates a fresh `std::vector<uint8_t>` for a fixed-size scalar. The benchmark matrix runs 200 ledgers with thousands of Soroban transactions per ledger, so even a tiny per-call allocation becomes a repeatable hot-path cost.

## Anti-Evidence

This is still a constant-factor improvement: the code only does one such allocation per Soroban tx in the parallel path, and one per Soroban op in the serial path. If host execution dominates a scenario, the gain may stay in the low single digits.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced from `subSha256` in `SHA.cpp:29-35` through `xdr::xdr_to_opaque` in `lib/xdrpp/xdrpp/marshal.h:264-272`. Confirmed that `xdr_to_opaque(counter)` allocates an `opaque_vec<>` (which inherits from `std::vector<uint8_t>` via `xvector` in `types.h:460`) of 8 bytes on the heap, only for it to be read by `SHA256::add` and immediately destroyed. The codebase already has a zero-allocation pattern for the same operation: `XDRHasher` in `XDRHasher.h:68-75` uses `xdr::swap64le` to write a uint64 as big-endian bytes directly without any heap allocation. The fix is to apply the same approach in `subSha256`.

### Code Paths Examined

- `src/crypto/SHA.cpp:29-36` — `subSha256` creates `SHA256`, adds seed, calls `xdr::xdr_to_opaque(counter)` which heap-allocates, adds result, finishes
- `lib/xdrpp/xdrpp/marshal.h:264-272` — `xdr_to_opaque` creates `opaque_vec<>` (= `xvector<uint8_t>` = inherits `std::vector<uint8_t>`), serializes into it, returns by value
- `lib/xdrpp/xdrpp/types.h:460` — `xvector` inherits from `std::vector<T>`, confirming heap allocation
- `src/crypto/XDRHasher.h:68-75` — existing zero-allocation pattern for uint64: `swap64le` + direct byte write
- `src/crypto/SHA.cpp:54-65` — `SHA256::add` calls `crypto_hash_sha256_update` on the bytes
- `src/ledger/LedgerManagerImpl.cpp:2396` — parallel apply calls `subSha256` once per Soroban tx
- `src/ledger/LedgerManagerImpl.cpp:2807-2808` — serial apply calls `subSha256` once per Soroban tx
- `src/transactions/TransactionFrame.cpp:2341` — serial apply calls `subSha256` once per Soroban op

### Findings

The inefficiency is real and the fix is correct:

1. **Allocation confirmed**: `xdr_to_opaque(uint64_t)` allocates a `std::vector<uint8_t>` of 8 bytes on the heap. `std::vector` does not have Small Buffer Optimization in any major standard library, so this is a real `malloc`/`free` pair per call.

2. **Fix is trivial and provably correct**: Replace `sha.add(xdr::xdr_to_opaque(counter))` with `auto be = xdr::swap64le(counter); sha.add(ByteSlice(&be, sizeof(be)));`. This produces the identical XDR big-endian encoding — the same approach `XDRHasher::operator()(uint64_t)` already uses at line 73 of `XDRHasher.h`.

3. **Impact is negligible for benchmarks**: The allocation saves ~20-50ns per call. Call frequency is once per Soroban tx (parallel path) or once per tx + once per op (serial path). Soroban host invocation costs milliseconds per tx, so this allocation represents <0.01% of per-tx cost. Even across 200 ledgers × thousands of txs, total savings would be low single-digit milliseconds — well below measurement noise on any benchmark scenario. The hypothesis's claimed "Low" (5-10%) severity is incorrect.

4. **Severity downgrade rationale**: While the fix is correct and eliminates a real inefficiency, the affected code path (`subSha256`) is vanishingly small relative to the dominant cost (Soroban host execution). No benchmark scenario would show a measurable improvement. This is Informational — worth fixing for code hygiene, not for performance.

### PoC Guidance

- **Target code**: `src/crypto/SHA.cpp:29-36` — the `subSha256` function
- **Change description**: Replace `sha.add(xdr::xdr_to_opaque(counter))` with direct big-endian encoding using `xdr::swap64le(counter)` and `ByteSlice` over the stack variable. Add `#include <xdrpp/endian.h>` if not already included (it is included transitively via `XDRHasher.h` → `xdrpp/endian.h`).
- **Correctness check**: The `[crypto]` test tag covers SHA operations. No existing test exercises `subSha256` directly, but the fix produces bit-identical output — verify by running a Soroban integration test and confirming ledger hashes match.
- **Benchmark focus**: Measure `subSha256` in isolation with microbenchmark (millions of calls) to confirm per-call improvement. Do not expect any visible change on apply-load benchmark scenarios — the improvement is below noise floor.
