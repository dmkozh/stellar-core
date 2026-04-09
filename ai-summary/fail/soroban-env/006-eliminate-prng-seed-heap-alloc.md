# H006: Eliminate Per-TX basePrngSeed CxxBuf Heap Allocation

**Date**: 2025-07-22
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: Avoid heap allocation for fixed-size 32-byte PRNG seed
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

The `basePrngSeed` (a 32-byte SHA-256 hash) should be passed to the Rust
bridge without heap allocation. Since it is a fixed-size array, it could be
passed as a `rust::Slice<const uint8_t>` or a fixed-size array type rather
than a heap-allocated `CxxBuf`.

## Mechanism

In `invokeHostFunction()` (InvokeHostFunctionOpFrame.cpp:540-543), each TX
constructs a `basePrngSeedBuf` by:
1. Creating an empty CxxBuf with `make_unique<vector<uint8_t>>()` (heap alloc)
2. Calling `assign(begin, end)` to copy 32 bytes from the hash

This is a heap allocation (~50ns) + 32-byte memcpy (~1ns) per TX. For 6400
SAC TXs: ~320μs total. Against ~850ms T=1 baseline: ~0.04%.

The fix would change the CXX bridge interface to accept `basePrngSeed` as a
`[u8; 32]` or `rust::Slice<const u8>` instead of `CxxBuf`, eliminating the
heap allocation entirely.

## Trigger

Run apply-load with any scenario. Every TX creates a CxxBuf for basePrngSeed.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:540-543` — basePrngSeedBuf creation
- `src/rust/src/bridge.rs:85-92` — `invoke_host_function` signature where `base_prng_seed: CxxBuf`

## Evidence

- The basePrngSeed is always exactly 32 bytes (SHA-256 hash)
- Using `make_unique<vector<uint8_t>>` for 32 bytes is wasteful (vector has ~24 bytes overhead + heap allocation)
- The Rust side only reads the bytes via `as_slice()` — no ownership needed

## Anti-Evidence

- The saving is ~50ns per TX (~320μs total for 6400 TXs) — negligible
- Changing the CXX bridge interface requires coordinated C++/Rust changes
- CxxBuf is the standard type for bridge data passing — special-casing one field adds inconsistency
- Similar micro-optimizations in this subsystem have been rejected as below noise floor

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-22
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The per-TX saving of ~50ns is 3-4 orders of magnitude below the measurable
threshold. Even at 6400 TXs, the total saving (~320μs) is ~0.04% of
benchmark time. This is deep in the noise floor.

### Lesson Learned

Fixed-size small allocations (~32 bytes) via `make_unique<vector<uint8_t>>`
are cheap on modern allocators (~50ns). Optimizing them is not worthwhile
unless called millions of times. The apply-load benchmark processes thousands,
not millions, of TXs.
