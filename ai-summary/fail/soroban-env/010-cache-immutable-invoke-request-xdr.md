# H001: Cache Immutable Invoke Request XDR Before Timed Apply

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Medium
**Impact**: CPU / bridge marshaling
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once an invoke-host-function transaction has been generated and validated, the
bridge should enter apply with serialized XDR for all tx-immutable request
fields already available. The measured apply path should reuse cached bytes for
the host function, Soroban resources, source account, and auth entries instead
of re-running `xdr_to_opaque` over the same immutable XDR tree for every
invocation.

## Mechanism

`InvokeHostFunctionApplyHelper::invokeHostFunction()` currently rebuilds the
request payload on every apply by serializing `hostFunction`, `resources`,
`sourceID`, and each auth entry into fresh `CxxBuf`s immediately before the FFI
call. In the apply-load benchmark, transaction generation and `checkValid()`
happen before the timer starts, and the same `TransactionFrameBasePtr` objects
are then passed into `closeLedger()`, so this serialization is hot-path work
that could be shifted onto immutable tx/op-frame caches without changing host
behavior.

## Trigger

Run `scripts/run_apply_load_matrix.py` with the existing benchmark configs.
`custom_token` and `soroswap` should show the clearest effect because their
invoke requests include both auth trees and non-trivial argument / footprint
XDR, while batched SAC (`batch_transfer`) also serializes a large destination
vector into the request on every apply.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:526-553` — per-apply `toCxxBuf(...)` of `hostFunction`, `resources`, `sourceID`, and auth entries just before `rust_bridge::invoke_host_function`
- `src/transactions/InvokeHostFunctionOpFrame.h:24-84` — plausible place to cache per-op serialized request fields
- `src/simulation/ApplyLoad.cpp:1958-2002` — benchmark timer starts only around `closeLedger(txs)`, after tx generation
- `src/simulation/ApplyLoad.cpp:2136-2148`
- `src/simulation/ApplyLoad.cpp:2336-2342`
- `src/simulation/ApplyLoad.cpp:3201-3205` — generated Soroban txs are validated before timing begins
- `src/simulation/TxGenerator.cpp:738-810` — SAC transfer request shape
- `src/simulation/TxGenerator.cpp:815-884` — custom-token transfer request shape
- `src/simulation/TxGenerator.cpp:1449-1520` — batched SAC request with large destination vector

## Evidence

The benchmark harness generates transactions, validates them, resolves bucket
futures, and only then snapshots the close timer before calling
`closeLedger(txs)`. But `invokeHostFunction()` still serializes the same
immutable tx payloads again inside apply, even though those fields were already
constructed earlier on the exact `TransactionFrameBasePtr` objects being
replayed. For `soroswap`, the generator also builds nested auth invocations and
multi-argument contract calls before timing starts, making repeated apply-time
serialization especially wasteful.

## Anti-Evidence

Rust-side deserialization inside `e2e_invoke` would still remain, so this only
removes the C++ half of the round trip. Caching the serialized request bytes on
transaction / operation frames increases memory footprint, and non-benchmark
paths that create-and-apply a tx immediately would benefit less because the
pre-serialization would stay on the same critical path.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the full path from `InvokeHostFunctionApplyHelper::invokeHostFunction()` (lines 526–553) through `toCxxBuf<T>()` (TransactionUtils.h:372–376) which calls `xdr::xdr_to_opaque(t)` to serialize each field. Confirmed that `hostFunction` (HostFunction XDR, ~170–350 bytes for InvokeContract), `resources` (SorobanResources with full LedgerFootprint, ~500–3000 bytes depending on footprint size), `sourceID` (AccountID, ~36 bytes), and auth entries (SorobanAuthorizationEntry, ~200–2000 bytes each) are serialized per-tx during apply. However, each transaction is applied exactly once — there is no repeated serialization of the same transaction. The hypothesis conflates "immutable fields that were constructed before timing" with "fields that were previously serialized," but no prior serialization exists to reuse.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:526-553` — `invokeHostFunction()`: calls `toCxxBuf()` on `hostFunction`, `resources`, `sourceID`, and each auth entry, producing 3 + N CxxBuf allocations per tx
- `src/transactions/TransactionUtils.h:372-376` — `toCxxBuf<T>()`: wraps `xdr::xdr_to_opaque(t)` in `CxxBuf{std::make_unique<std::vector<uint8_t>>(...)}`; a single-pass XDR write into a freshly-allocated vector
- `src/protocol-curr/xdr/Stellar-transaction.h:3165-3167` — `InvokeHostFunctionOp` struct: contains `HostFunction hostFunction` and `xvector<SorobanAuthorizationEntry> auth`
- `src/protocol-curr/xdr/Stellar-transaction.h:4940-4944` — `SorobanResources` struct: contains `LedgerFootprint footprint` (two xvectors of LedgerKey) plus 3 uint32 fields
- `src/protocol-curr/xdr/Stellar-transaction.h:3124-3126` — `SorobanAuthorizationEntry`: contains `SorobanCredentials` + `SorobanAuthorizedInvocation` tree
- `src/rust/src/soroban_proto_any.rs:443-458` — Rust side passes these encoded buffers through to `e2e_invoke::invoke_host_function()` where they are deserialized inside the soroban host (out of scope)

### Why It Failed

The per-tx serialization cost of the request fields is too small to produce a measurable benchmark improvement, for three compounding reasons:

1. **The serialized structures are small.** For SAC transfers: `HostFunction` ~200 bytes, `SorobanResources` ~600 bytes, `AccountID` ~36 bytes, 1 auth entry ~300 bytes — total ~1.1KB. For soroswap: ~3KB total with larger footprints and auth trees. XDR serialization of ~1–3KB of structured data costs ~0.5–3μs per transaction.

2. **Each transaction is serialized exactly once.** Unlike the cost params optimization (success #001) where the same ledger-wide data was serialized redundantly for every tx, these request fields are unique per transaction. There is no cross-tx sharing opportunity. The proposal to pre-serialize during generation/validation merely shifts work outside the benchmark timing window — it does not eliminate redundant work.

3. **Aggregate impact is <0.5% of baseline.** SAC T=1: 6400 txs × ~0.7μs ≈ 4.5ms / 850ms ≈ 0.5%. SAC T=8: 800 txs/thread × ~0.7μs ≈ 0.56ms / 700ms ≈ 0.08%. Soroswap T=1: 1600 txs × ~2μs ≈ 3.2ms / 713ms ≈ 0.4%. All scenarios are well below the 5% Low severity threshold and within benchmark measurement noise.

Additionally, even with caching, each `CxxBuf` requires its own `unique_ptr<vector<uint8_t>>`, so cached bytes must be copied (memcpy) into new vectors. For these small structured XDR types, the savings from replacing field-by-field serialization with memcpy is only ~30–50% of the already-tiny serialization cost.

### Lesson Learned

Distinguish between **ledger-wide shared state** (like cost params, which are identical for every tx in a close — caching produces N×savings) and **per-tx unique state** (like request fields, which are serialized exactly once — caching only shifts work to an earlier point, with zero aggregate reduction). The confirmed success #001 worked because cost params are O(1)-per-ledger data serialized O(N)-per-tx times; this hypothesis targets O(N) data serialized O(1) times each.
