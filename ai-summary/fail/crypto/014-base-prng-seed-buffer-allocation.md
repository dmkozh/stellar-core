# H014: Fixed-Size PRNG Seed Buffer Allocation In The Bridge Helper

**Date**: 2026-04-10
**Subsystem**: crypto, rust
**Severity**: Low
**Impact**: tiny per-tx heap allocation
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Passing the 32-byte per-tx Soroban PRNG seed into Rust should avoid a heap
allocation if that allocation materially affects the measured apply path. A
fixed-size seed should ideally travel via stack storage or an in-place bridge
field rather than a freshly allocated `std::vector<uint8_t>`.

## Mechanism

`InvokeHostFunctionApplyHelper::invokeHostFunction` builds a new `CxxBuf`,
allocates a `std::vector<uint8_t>`, and copies the 32-byte `mSorobanBasePrngSeed`
into it for every host invocation. If this allocation were large enough to
matter, replacing it with a fixed-size bridge payload would remove one heap
round-trip per Soroban tx.

## Trigger

Run any apply-load scenario and compare allocator samples in the PRNG-seed setup
path against a build that passes the seed without allocating a `std::vector`.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:592-595` — allocate and fill `basePrngSeedBuf`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:601-610` — pass the buffer into `rust_bridge::invoke_host_function`

## Evidence

The code performs a heap allocation per host invocation for data whose size is
always 32 bytes. This is a real extra allocation in the measured apply path.

## Anti-Evidence

The seed allocation is tiny compared with the surrounding work in the same
function: serializing `SorobanResources`, auth entries, ledger-entry batches,
and the host execution itself. Even at thousands of txs per ledger, saving one
32-byte allocation per tx does not plausibly reach the 5% benchmark threshold.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The mechanism is real but far too small. This path saves exactly one tiny
allocation and one 32-byte copy per Soroban invoke, while the surrounding
bridge setup already performs much larger XDR serializations and buffer
construction on every tx.

### Lesson Learned

For apply-load, fixed-size scalar bridge payloads are rarely worth chasing on
their own. The measurable wins are much more likely to come from eliminating
whole-buffer serialization patterns or O(N) per-entry bridge allocations.
