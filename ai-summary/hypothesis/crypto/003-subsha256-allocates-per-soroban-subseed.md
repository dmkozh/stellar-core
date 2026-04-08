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
