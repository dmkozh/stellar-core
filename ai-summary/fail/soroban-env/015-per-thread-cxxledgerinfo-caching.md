# H015: Per-Thread CxxLedgerInfo Caching with FFI Reference Passing

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Low
**Impact**: CPU / FFI input marshaling
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`CxxLedgerInfo` is identical for all transactions in a ledger close. It should
be constructed once per thread (or once per ledger) and passed by reference
through the FFI boundary, rather than being reconstructed per transaction.
Currently `getLedgerInfo()` constructs a new `CxxLedgerInfo` for every TX,
including two `toCxxBuf(costParams)` calls that serialize ~1720 bytes each,
plus network_id copy (32 bytes) and integer field assignments.

## Mechanism

The `InvokeHostFunctionParallelApplyHelper::getLedgerInfo()` method is called
once per TX and constructs a fresh `CxxLedgerInfo` containing:
- `cpu_cost_params`: `toCxxBuf(mSorobanConfig.cpuCostParams())` — serializes
  86 `ContractCostParamEntry` structs (~1720 bytes XDR)
- `mem_cost_params`: `toCxxBuf(mSorobanConfig.memCostParams())` — same size
- `network_id`: 32-byte hash copy
- ~10 integer fields (ledger sequence, version, timestamps, etc.)

The two cost param serializations dominate at ~1-2μs each. Caching the entire
`CxxLedgerInfo` per thread would save ~2-4μs per TX. With 6400 TXs across 8
threads: ~800 TXs per thread × ~3μs = ~2.4ms per thread, ~2.4ms total
(parallel). Against 850ms baseline: ~0.3%.

## Trigger

Run any apply-load benchmark scenario with Soroban transactions.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:41-70` — `getLedgerInfo()` constructs CxxLedgerInfo per TX
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf` template performs XDR serialization

## Evidence

The CxxLedgerInfo content is purely derived from ledger-level configuration
that doesn't change between transactions within a single ledger close.

## Anti-Evidence

Reviewed H001 already proposes caching the serialized cost params bytes on the
C++ side, which addresses the most expensive component. The remaining overhead
(network_id copy + integer fields) is ~50-100ns per TX, well below the noise
floor.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PARTIAL — overlaps significantly with reviewed H001

### Why It Failed

Reviewed H001 (cache-serialized-cost-params-cpp-side) already targets the
dominant cost within getLedgerInfo(): the two `toCxxBuf(costParams)` calls.
After H001 is implemented, the remaining per-TX CxxLedgerInfo construction
cost is ~50-100ns (copying integers and a 32-byte hash), which is well below
the measurability threshold. Proposing the broader "cache entire CxxLedgerInfo"
adds no meaningful savings beyond what H001 already captures. Additionally,
passing CxxLedgerInfo by reference through the CXX FFI boundary requires
non-trivial bridge API changes (CXX doesn't support arbitrary reference
parameters across the FFI boundary for opaque types).

### Lesson Learned

When an existing hypothesis already targets the dominant cost component of a
function, broader caching proposals for the same function provide minimal
additional value. Check reviewed hypotheses for overlap before proposing.
