# H004: Pre-Marshal Immutable Invoke-Host Inputs Before the Timed Apply Path

**Date**: 2026-04-10
**Subsystem**: transaction-ledger
**Severity**: Medium
**Impact**: Per-tx C++↔Rust marshaling overhead before Soroban host execution
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Immutable parts of an invoke-host call should be serialized once per transaction
object and reused at apply, rather than being re-encoded into fresh `CxxBuf`
objects immediately before every host invocation.

## Mechanism

`InvokeHostFunctionApplyHelper::invokeHostFunction` rebuilds `authEntryCxxBufs`
and re-serializes the host function, `SorobanResources`, and source account into
fresh `CxxBuf` objects on every apply-time call. Those payloads are immutable
for the lifetime of the `TransactionFrame`, and apply-load both constructs and
validates the txs before the benchmark timer starts. That means a substantial
slice of per-tx bridge setup — especially auth trees and larger model-tx
argument lists such as soroswap and batch SAC — can be shifted off the hot path
by caching pre-marshaled blobs in `InvokeHostFunctionOpFrame` or another
transaction-owned side structure.

## Trigger

Run `scripts/run_apply_load_matrix.py` and sample time/allocations in
`toCxxBuf`, `xdr::xdr_to_opaque`, and `InvokeHostFunctionApplyHelper::invokeHostFunction`.
The signal should be strongest on `soroswap` and batched `sac`, where auth and
argument payloads are larger than the simple single-transfer case.

## Target Code

- `src/transactions/TransactionFrame.cpp:107-118` — `TransactionFrame` constructs `OperationFrame` objects once per tx, giving a natural cache lifetime
- `src/transactions/InvokeHostFunctionOpFrame.h:24-36` — `InvokeHostFunctionOpFrame` currently stores only references, not pre-marshaled call inputs
- `src/transactions/InvokeHostFunctionOpFrame.cpp:529-553` — apply-time bridge call rebuilds auth buffers and re-serializes immutable invoke inputs
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1214-1218` — invoke-host op construction site where immutable caches could be initialized lazily
- `src/simulation/ApplyLoad.cpp:1955-2004` — benchmark timing excludes tx generation and starts just before `closeLedger`
- `src/simulation/ApplyLoad.cpp:2136-2148` — SAC benchmark validates txs before timing
- `src/simulation/ApplyLoad.cpp:2336-2341` — custom-token benchmark validates txs before timing
- `src/simulation/ApplyLoad.cpp:3120-3189` — soroswap builds a larger invoke-contract payload and nested auth tree
- `src/simulation/ApplyLoad.cpp:3201-3206` — soroswap txs are validated before timing

## Evidence

- The current bridge call allocates and fills multiple fresh `CxxBuf`s per tx
  even though the underlying XDR fields are immutable and already resident in
  the `TransactionFrame`.
- Apply-load places tx generation and validation before the measured close, so
  this setup work is a good candidate for hoisting without changing benchmark
  semantics.
- Soroswap and batch-SAC model txs include richer args/auth than minimal token
  transfers, making the per-tx `xdr_to_opaque` cost more visible.

## Anti-Evidence

- Caching serialized blobs increases memory footprint per transaction, so the net
  win depends on whether saved apply-time CPU outweighs the extra retained memory.
- Live-network tx objects may not always survive long enough between construction
  and apply to amortize the cache as well as apply-load does.
- The per-call PRNG seed is still dynamic and would remain uncached unless the
  bridge API also learns to accept a non-owning 32-byte view.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (distinct from fail/021 which targeted ledger entry CxxBufs, and fail/017 which targeted CxxLedgerInfo cost params)
**Failed At**: reviewer

### Trace Summary

Traced the complete `invokeHostFunction` path at lines 525-553 of InvokeHostFunctionOpFrame.cpp. The four targeted serializations — `authEntryCxxBufs` (line 529-534), `toCxxBuf(hostFunction)` (line 548), `toCxxBuf(mResources)` (line 549), and `toCxxBuf(sourceAccount)` (line 550) — each call `xdr::xdr_to_opaque` to serialize XDR into a heap-allocated `vector<uint8_t>` wrapped in a `CxxBuf`. Measured the actual payload sizes from the soroswap benchmark tx construction (ApplyLoad.cpp:3120-3189): soroswap auth is ~350-500 bytes (one root invocation + one transfer sub-invocation), hostFunction is ~300 bytes, SorobanResources is ~800-1200 bytes (5 RO + 5 RW footprint keys), and sourceAccount is ~36 bytes. SAC transfers are smaller across all four fields.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:525-553` — `invokeHostFunction()` serializes 4 immutable tx-level inputs into CxxBufs; these are the targets
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf<T>` calls `xdr_to_opaque` + `make_unique<vector<uint8_t>>`; cost is ~1ns/byte serialization + ~100-200ns heap alloc
- `src/simulation/ApplyLoad.cpp:3120-3189` — soroswap tx has 5 RO + 5 RW footprint keys, nested 2-level auth tree, 5-arg InvokeContractArgs
- `src/simulation/ApplyLoad.cpp:2100-2148` — SAC tx has simpler footprint (3-5 keys), single auth entry, 3-arg transfer
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1214-1218` — constructor stores only a const reference to the op XDR; would need `mutable` members for caching
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1282-1311` — `doCheckValidForSoroban` does NOT serialize these inputs (only checks wasm size/asset validity), so there is no duplication between validation and apply

### Why It Failed

The per-tx serialization cost for these four inputs is ~1-3µs (soroswap: ~2000 bytes × ~1ns/byte + 4-5 heap allocs × ~150ns ≈ ~2.7µs; SAC: ~700 bytes + 4 allocs ≈ ~1.3µs). At maximum benchmark scale (3200 SAC or 1000 soroswap txs), the cumulative savings are ~3-5ms per ledger — well under 1% of close time (200-800ms). This is below the 5% Low threshold and comparable to the per-tx savings that fail/021 already tried (~4µs per tx via batched CxxBuf allocations) and found unmeasurable in the benchmark.

Additionally, the hypothesis frames this as "shifting work off the hot path" rather than eliminating it. The serialization only happens once per tx during apply — there is no duplicate serialization between validation and apply for these specific fields (`doCheckValidForSoroban` never serializes them). The "optimization" would move ~1-3µs of work from apply time to construction time, which benefits only the benchmark (where construction precedes the timed window) but not the live network (where total CPU work is unchanged). Caching also requires adding `mutable` members with heap-allocated `CxxBuf` objects to a const-qualified class, increasing per-tx memory footprint for a sub-1% timing benefit.

### Lesson Learned

Per-tx XDR serialization of small-to-moderate payloads (< 2KB) costs ~1-3µs, which is below the benchmark noise floor even at 3200 txs. This converges with fail/021's finding that per-tx CxxBuf allocation savings of ~4µs are unmeasurable. To meaningfully reduce per-tx bridge setup cost, the target must be either (a) large payloads serialized repeatedly (like the cost params in success/soroban-env/001 at ~1.7KB × per-tx × Rust-side deserialization), or (b) operations that can be amortized across many transactions (ledger-level caching).
