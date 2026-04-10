# H019: Batch Auth Bridge Inputs To Avoid Per-Entry Serialization Overhead

**Date**: 2026-04-10
**Subsystem**: crypto, rust
**Severity**: Low
**Impact**: bridge allocation overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If apply-load transactions routinely carried several Soroban authorization
entries, the bridge should batch those auth blobs the same way as any other
variable-length input collection so that the measured apply path does not pay
one allocation and cxx wrapper per auth entry.

## Mechanism

`InvokeHostFunctionApplyHelper::invokeHostFunction` builds a fresh
`rust::Vec<CxxBuf>` for `mInvokeHostFunction.auth` and serializes each entry via
`toCxxBuf(authEntry)`. If the benchmark workloads carried many auth entries per
transaction, a batched `data + lengths` representation could remove per-entry
allocation and cxx bookkeeping.

## Trigger

Run `custom_token` and `soroswap` apply-load and inspect the number of auth
entries per transaction alongside allocator samples in the auth serialization
loop.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:529-534` — hot-path auth entries are serialized one-by-one into `Vec<CxxBuf>`
- `src/simulation/ApplyLoad.cpp:2280-2287` — `custom_token` mint tx adds exactly one auth entry
- `src/simulation/ApplyLoad.cpp:2681-2687` — soroswap factory init adds exactly one auth entry
- `src/simulation/ApplyLoad.cpp:2755-2760` — router init adds exactly one auth entry
- `src/simulation/ApplyLoad.cpp:3040-3045` — add-liquidity tx adds exactly one auth entry
- `src/simulation/ApplyLoad.cpp:3170-3189` — swap tx adds exactly one auth entry

## Evidence

The auth serialization loop is real, and the encoded auth payload can be large
because a single auth entry may contain nested sub-invocations. The benchmark
generator, however, consistently populates `invokeHostFunctionOp().auth` with
exactly one `emplace_back(...)` per transaction in the scenarios that matter for
apply-load.

## Anti-Evidence

That single auth entry can still be large, so there is some serialization work
on the hot path. But batching a collection of size 1 does not remove the unique
XDR encoding or the single bridge object that must still cross FFI.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The apply-load scenarios only carry one auth entry per Soroban transaction, so
there is no meaningful per-entry collection overhead to amortize. A batched auth
format would still have to serialize and ship that single unique auth blob, so
the saved work is limited to one outer-container object per tx.

### Lesson Learned

Before proposing batching for a bridge collection, confirm the benchmark
actually sends multi-item collections. For apply-load, auth complexity is mostly
*inside* one `SorobanAuthorizationEntry`, not in the number of entries.
