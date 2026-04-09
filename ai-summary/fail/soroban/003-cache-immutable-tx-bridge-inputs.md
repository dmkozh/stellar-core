# H001: Cache Immutable Tx Bridge Inputs Across Apply

**Date**: 2026-04-09
**Subsystem**: soroban
**Severity**: Medium
**Impact**: CPU / allocation churn in C++->Rust bridge marshaling
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

For a Soroban transaction, immutable inputs to the Rust bridge such as the host
function XDR, `SorobanResources`, source account, and authorization entries
should be serialized at most once per transaction and then reused during apply.
Repeated bridge invocations should spend their CPU budget on host execution and
ledger access, not on rebuilding identical `std::vector<uint8_t>` payloads.

## Mechanism

`InvokeHostFunctionApplyHelper::invokeHostFunction` rebuilds fresh `CxxBuf`
wrappers for `hostFunction`, `mResources`, `getSourceID()`, and every auth
entry on every apply, and `toCxxBuf` always allocates and runs
`xdr::xdr_to_opaque`. The Rust side only consumes these as immutable byte
slices, so this work is redundant and sits directly in the benchmarked apply
path; large footprints and auth trees in `custom_token` and `soroswap` should
amplify the waste.

## Trigger

Run the apply-load matrix on `custom_token` or `soroswap`, especially `T=8`,
and profile CPU time around `InvokeHostFunctionApplyHelper::invokeHostFunction`.
If the hypothesis is correct, a measurable share of per-tx time will be spent in
`xdr::xdr_to_opaque` and vector allocation before `rust_bridge::invoke_host_function`
starts executing the host.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::invokeHostFunction:526-553` — reserializes host function, resources, source account, and auth entries per apply
- `src/transactions/TransactionUtils.h:toCxxBuf:370-375` — always allocates a new `std::vector<uint8_t>` from `xdr::xdr_to_opaque`
- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:443-457` — consumes these values only as encoded byte slices

## Evidence

The hot call site constructs a new `rust::Vec<CxxBuf>` for auth entries and
passes newly serialized `CxxBuf`s for every other immutable tx-side argument.
There is no memoized encoded representation on `InvokeHostFunctionOpFrame` or
its parent transaction object, even though these XDR objects are immutable after
transaction construction.

## Anti-Evidence

Each transaction only invokes the host once during apply, so the win depends on
the serialized footprint/auth payloads being large enough to matter relative to
host execution. Small SAC transfers with minimal auth may see a smaller gain
than router/pool paths with larger arguments and auth trees.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the complete apply path for `InvokeHostFunctionOpFrame`. Each Soroban
transaction has exactly one operation, and `invokeHostFunction()` is called
exactly once per transaction apply (line 895 in `doApply()`). The `toCxxBuf`
calls at lines 548–550 serialize the host function, resources, and source
account once each. Auth entries are serialized once per entry at lines 531–534.
There is no second call site — neither `doCheckValidForSoroban` nor
`doParallelApply` re-serializes these inputs.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:doApply:884-918` — calls `invokeHostFunction(out)` exactly once
- `src/transactions/InvokeHostFunctionOpFrame.cpp:invokeHostFunction:526-553` — serializes host function, resources, source account, auth entries via `toCxxBuf`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:doApplyForSoroban:1229-1248` — creates helper, calls `helper.apply()` once
- `src/transactions/InvokeHostFunctionOpFrame.cpp:doParallelApply:1260-1279` — creates helper, calls `helper.apply()` once
- `src/transactions/InvokeHostFunctionOpFrame.cpp:doCheckValidForSoroban:1282-1310` — does NOT serialize any bridge inputs
- `src/transactions/TransactionUtils.h:toCxxBuf:372-376` — allocates and serializes XDR to bytes
- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:391-462` — receives encoded byte slices, deserializes them inside the host

### Why It Failed

The premise of "redundant serialization" is incorrect. Each `toCxxBuf` call
happens exactly once per transaction per input — there is no repeated
serialization to eliminate. The hypothesis title says "cache across apply" but:

1. **No intra-transaction redundancy**: `invokeHostFunction()` is called once
   per transaction apply. The serialization is a one-shot cost, not repeated.
2. **No cross-transaction caching opportunity**: Each transaction has unique
   host function args, resources, source account, and auth entries — nothing
   is shared across transactions.
3. **The serialization is inherent to the FFI boundary**: The C++ → Rust bridge
   requires byte buffers (`CxxBuf`). XDR serialization is the necessary
   translation step; it cannot be skipped.
4. **Cost is negligible relative to host execution**: Serializing ~1–2 KB of
   XDR (host function + resources + source account + auth entries) takes
   single-digit microseconds. Soroban host execution for even a simple SAC
   transfer takes hundreds of microseconds to milliseconds.

### Lesson Learned

Soroban transactions are single-operation and invoke the host exactly once per
apply. The bridge serialization step is not redundant — it is a one-time cost
per transaction. When evaluating bridge marshaling overhead, verify the actual
call count before assuming repeated work. The real cost center is the host
execution itself, not the input marshaling.
