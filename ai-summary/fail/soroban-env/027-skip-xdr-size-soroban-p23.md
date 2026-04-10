# H027: Skip `xdr_size(lk)` Computation for Soroban Entries on p23+ in addReads

**Date**: 2025-07-18
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: CPU / redundant computation
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

For soroban entries on protocol ≥23, `addReads` should not compute `xdr_size(lk)`
since the result is never used — `meterDiskReadResource` is skipped for soroban
entries on p23+ (lines 487-494 guard this). The `keySize` variable should only
be computed when it will actually be consumed.

## Mechanism

In `addReads` (InvokeHostFunctionOpFrame.cpp:372), `xdr_size(lk)` is computed
for every key in the footprint. However, for soroban entries on p23+, the only
consumer of `keySize` is `meterDiskReadResource` (line 491), which is skipped
per the protocol version check on lines 487-489. The `keySize` is computed but
never read, wasting ~10-20 ns per key per TX.

## Trigger

Run any apply-load benchmark (SAC @ 3200 TXs). With ~10-20 footprint keys per
TX: 3200 × 15 keys × 15 ns = ~720 μs per ledger. At baseline ~2500 ms (T=1):
~0.03%. At T=8 ~350 ms: ~0.21%.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:372` — `xdr_size(lk)` computed unconditionally
- `src/transactions/InvokeHostFunctionOpFrame.cpp:487-494` — `meterDiskReadResource` guarded by protocol version check for soroban entries

## Evidence

The code computes `keySize` at line 372 but only uses it at line 491 inside a
condition that is false for soroban entries on p23+. The xdr_size computation
walks the XDR struct to compute serialized size — cheap but unnecessary.

## Anti-Evidence

`xdr_size` for a `LedgerKey` is extremely cheap (~10-20 ns), making the total
savings ~720 μs across 3200 TXs × 15 keys. This is well below the benchmark
noise floor.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-18
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The per-key cost of `xdr_size` is ~10-20 ns, and the total savings across a
full ledger (~720 μs) represent <0.03% at T=1 and <0.21% at T=8. This is
firmly below the benchmark noise floor (~1-3% run-to-run variation) and below
the Informational severity threshold for actionability.

### Lesson Learned

Compile-time-resolvable redundant computations in the bridge layer (like
`xdr_size` for fixed-structure keys) are individually too cheap to matter.
The per-key cost model (~10-20 ns) is dominated by CPU pipeline and cache
effects, making elimination unmeasurable even at scale.
