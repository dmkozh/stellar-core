# H001: Share Immutable Read-Only Input Buffers Across Transactions

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Low
**Impact**: CPU / input marshaling
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

For apply-load workloads, contract-code / contract-instance footprint entries
that are identical across many transactions should be serialized once per
thread (or once per ledger close) and then reused by the bridge. The Rust side
only needs borrowed bytes for these input entries, so immutable read-only
buffers should not be re-serialized and re-owned on every invocation.

## Mechanism

The bridge currently models every input XDR blob as an owned `CxxBuf` holding a
`UniquePtr<CxxVector<u8>>`, and `addReads()` serializes each read-only footprint
entry with `toCxxBuf(*entryOpt)` on every transaction. In the benchmark
workloads, those read-only entries repeat heavily: SAC and custom-token
transfers reuse `instance.readOnlyKeys`, and batched SAC explicitly prepends the
same batch-transfer and SAC read-only key sets to every transaction.

Because `CxxBuf` is uniquely owned, even a cache of serialized bytes cannot be
reused zero-copy today; each invocation has to materialize a fresh owned buffer.
If the bridge used a shared immutable byte container (or a borrowed-input
variant distinct from output buffers), the helper could cache serialized
read-only entries and pass the exact same bytes to Rust across all transactions.

## Trigger

Run the apply-load matrix on `custom_token` or `soroswap`, or the batched SAC
scenario (`APPLY_LOAD_BATCH_SAC_COUNT = 100`). The same contract code /
instance read-only footprint entries are marshaled across the C++↔Rust boundary
for every transaction in the ledger close.

## Target Code

- `src/rust/src/bridge.rs:13-15` — `CxxBuf` forces owned `UniquePtr<CxxVector<u8>>` storage
- `src/rust/src/bridge.rs:193-208` — `invoke_host_function` accepts input buffers that Rust only reads
- `src/rust/src/common.rs:9-12` — Rust side consumes `CxxBuf` as a borrowed byte slice
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf()` creates a fresh owned byte buffer for every XDR object
- `src/transactions/InvokeHostFunctionOpFrame.cpp:360-466` — `addReads()` serializes every footprint entry for every invocation
- `src/simulation/TxGenerator.cpp:761-767` — SAC transfers reuse `instance.readOnlyKeys`
- `src/simulation/TxGenerator.cpp:840-845` — custom-token transfers reuse `instance.readOnlyKeys`
- `src/simulation/TxGenerator.cpp:1486-1490` — batched SAC appends the same two read-only key sets to every tx

## Evidence

`CxxBuf` ownership is stronger than the call site needs: `common.rs` shows Rust
only calls `as_slice()` on input buffers. Meanwhile `addReads()` and
`toCxxBuf()` serialize the same immutable read-only entries for every
invocation, even though the workload generators build transactions that share
the same contract code / instance keys ledger-wide. This is exactly the kind of
repeated bridge work that batched SAC and multi-contract soroswap amplify.

## Anti-Evidence

The change likely needs a new bridge type or an opaque shared-byte wrapper,
because `cxx` does not currently let this code reuse the existing
`UniquePtr<CxxVector<u8>>` representation. Read-write entries would still need
per-transaction materialization, so the win is concentrated in scenarios whose
footprints are dominated by repeated read-only contract code / instance blobs.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — duplicate of 008-cache-serialized-readonly-entries-per-thread.md
**Failed At**: reviewer

### Trace Summary

This hypothesis is substantially equivalent to the already-investigated and
rejected hypothesis 008 ("Cache Pre-Serialized ReadOnly Footprint Entries Per
Thread"). Both target the same code path (`addReads()` calling `toCxxBuf()` per
TX per read-only footprint key), the same workload pattern (shared contract
code/instance entries across TXs), and propose the same core idea (cache or
share the serialized bytes). The only new element is proposing a shared/borrowed
buffer type instead of memcpy from a cache, but this does not change the
magnitude of savings.

### Code Paths Examined

- `src/rust/src/bridge.rs:13-15` — `CxxBuf` struct wraps `UniquePtr<CxxVector<u8>>`; confirmed Rust only calls `as_ref()` → `as_slice()` on input buffers
- `src/rust/src/bridge.rs:193-208` — `invoke_host_function` takes `ledger_entries: &Vec<CxxBuf>` by reference
- `src/rust/src/soroban_proto_any.rs:443-458` — the `encoded_ledger_entries: I` iterator passes `T: AsRef<[u8]>` to `e2e_invoke::invoke_host_function_with_trace_hook`, which XDR-deserializes each entry into Rust objects (this Rust-side deserialization cost is unchanged by the proposal)
- `src/transactions/InvokeHostFunctionOpFrame.cpp:448-466` — `addReads()` inner loop: `getLedgerEntryOpt(lk)` → `toCxxBuf(*entryOpt)` per TX per read-only key

### Why It Failed

1. **Duplicate of a prior investigation.** Hypothesis 008 already traced this
   exact code path and computed the aggregate savings ceiling. The conclusion:
   even with perfect elimination of all C++-side re-serialization, the total
   savings are <0.5% of the T=8 baseline (SAC: ~560μs/700ms, custom_token:
   ~2.8ms/700ms, soroswap: ~1.8ms/700ms), all well under the 5% Low threshold.

2. **Zero-copy does not change the math.** The hypothesis claims that a
   shared/borrowed buffer type would improve over the cached-then-copied
   approach rejected in 008. Even granting perfect zero-copy (no memcpy at all),
   the per-TX cost being saved is the full C++ XDR serialization (~0.35–7.5μs
   depending on entry size). But 008 already computed savings assuming full
   serialization elimination, and the aggregate still falls under 0.5%.

3. **Rust-side deserialization remains.** Even with zero-copy byte sharing on
   the C++ side, the Rust soroban host still XDR-deserializes each entry from
   the byte buffer into Rust objects inside `e2e_invoke`. This deserialization
   is at least as costly as C++ serialization. The proposal saves at most the
   C++ half of the round-trip — which 008 already showed is too small to matter.

4. **cxx limitations make zero-copy impractical.** The `cxx` crate does support
   `&[u8]` slices, but `invoke_host_function` takes `&Vec<CxxBuf>` where each
   `CxxBuf` owns a `UniquePtr<CxxVector<u8>>`. Changing the FFI signature to
   accept shared references would require either a new bridge type or
   restructuring the call to pass slices, adding complexity for unmeasurable gain.

### Lesson Learned

When a hypothesis proposes a different implementation strategy (zero-copy vs.
cached-copy) for the same underlying optimization, check whether the prior
investigation already computed an upper bound on savings that applies regardless
of implementation approach. In this case, 008 computed the total C++-side
serialization cost ceiling, which is the same whether eliminated by caching +
memcpy or by zero-copy sharing.
