# H001: Batch Footprint Bridge Inputs Instead Of Per-Entry `CxxBuf`s

**Date**: 2026-04-10
**Subsystem**: crypto, rust
**Severity**: Medium
**Impact**: per-tx FFI allocation and cxx marshalling overhead in measured Soroban apply
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The Soroban apply path should hand Rust the footprint ledger entries and TTL
entries as one contiguous payload plus offsets/lengths, not as a `Vec<CxxBuf>`
containing one heap-allocated `std::vector<uint8_t>` per item. The Rust side
already only needs an iterator of byte slices, so the hot path should not pay
O(N) `unique_ptr<CxxVector<u8>>` allocation and cxx wrapper overhead just to
reconstruct those slices.

## Mechanism

`InvokeHostFunctionApplyHelper::addReads` currently serializes every live
footprint entry into its own `CxxBuf` via `toCxxBuf(*entryOpt)`, and it does the
same for every TTL entry, including allocating an empty vector for entries with
no TTL. `invoke_host_function` then forwards `rust::Vec<CxxBuf>` collections
over cxx.rs, but the Rust side immediately consumes them as generic
`AsRef<[u8]>` iterators into `e2e_invoke::invoke_host_function`, meaning the
per-entry ownership boundary is pure bridge overhead. Replacing those vectors
with a batched `data + lengths` representation would remove many per-tx
allocations and pointer-chasing objects while preserving the downstream host API.

## Trigger

Run `custom_token` or `soroswap` apply-load, especially `T=8`, and sample
allocator / cxx bridge traffic during `InvokeHostFunctionApplyHelper::addReads`
and `invoke_host_function`. Compare against a build that serializes
`ledger_entries` and `ttl_entries` directly into one contiguous bridge buffer
per collection and exposes a Rust slice iterator over offsets.

## Target Code

- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf` always allocates a fresh `std::vector<uint8_t>`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:312-314` — per-tx vectors reserve entry counts, not payload storage
- `src/transactions/InvokeHostFunctionOpFrame.cpp:453-466` — each live entry and TTL entry becomes its own `CxxBuf`, including empty TTL vectors
- `src/transactions/InvokeHostFunctionOpFrame.cpp:544-553` — Rust bridge still takes `mLedgerEntryCxxBufs` and `mTtlEntryCxxBufs` as `Vec<CxxBuf>`
- `src/rust/src/bridge.rs:193-208` — FFI surface encodes footprint inputs as `&Vec<CxxBuf>`
- `src/rust/src/soroban_proto_any.rs:443-454` — Rust immediately passes `ledger_entries.iter()` / `ttl_entries.iter()` to the host
- `src/rust/src/soroban_proto_all.rs:101-135` — protocol dispatch only requires generic `AsRef<[u8]>` iterators, so batched slice iteration fits the existing host-side contract

## Evidence

The current code has no batch-input helper at all: every footprint item crosses
the bridge as its own `CxxBuf`, and even the "missing TTL" case allocates a
distinct empty vector. The Rust side does not need random access ownership of
each element; it just iterates through byte slices, which makes the current
representation substantially more expensive than the consumer requires.

## Anti-Evidence

This does not remove the need to XDR-serialize each unique ledger entry at least
once, so the win comes from allocation and bridge-object elimination rather than
eliminating all encoding work. The biggest gains should therefore show up in the
heavier `custom_token` and `soroswap` footprints, not uniformly across all
benchmarks.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — duplicate of transaction-ledger fail #021 ("Replace Per-Entry CxxBuf Heap Allocation with Batched FFI Input Buffers")
**Failed At**: reviewer

### Trace Summary

This hypothesis proposes replacing per-entry `CxxBuf` allocations for ledger entries and TTL entries with a batched contiguous buffer plus offsets/lengths. This is identical to the investigation in `ai-summary/fail/transaction-ledger/021-batch-ffi-input-buffers-for-invoke-host.md`, which went through full review (VIABLE at Informational), a complete PoC implementation (`CxxBatchBufBuilder`/`CxxBatchBuf`), successful test passage (all 109 Soroban tests, full `make check`), and independent benchmark validation — then was REJECTED at final-review because the benchmark results showed no meaningful improvement.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:448-466` — per-entry `toCxxBuf` calls in `addReads` for ledger entries and TTL entries (same code targeted by transaction-ledger #021)
- `src/transactions/InvokeHostFunctionOpFrame.cpp:544-553` — `invoke_host_function` call passing `mLedgerEntryCxxBufs` and `mTtlEntryCxxBufs` (same FFI boundary targeted)
- `src/rust/src/bridge.rs:193-208` — `invoke_host_function` accepting `&Vec<CxxBuf>` (the PoC already changed this to `&CxxBatchBuf`)
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf` allocating per-entry buffers (already replaced in PoC with `CxxBatchBufBuilder::append`)

### Why It Failed

1. **Exact duplicate of transaction-ledger #021.** That investigation proposed the identical optimization: replace per-entry `CxxBuf` allocations in `addReads` with a contiguous batch buffer. It was implemented as `CxxBatchBufBuilder` (C++ side) and `CxxBatchBuf` with `BatchBufIter` (Rust side), modifying `bridge.rs`, `common.rs`, `soroban_proto_any.rs`, `soroban_proto_all.rs`, and `InvokeHostFunctionOpFrame.cpp`.

2. **PoC was fully implemented and benchmarked.** The PoC passed all tests (109 Soroban tests, full `make check`). Independent `run_apply_load_matrix.py` results showed no meaningful improvement:
   - Best result: +2.48% p95 on soroswap T=8 (within noise)
   - Several scenarios regressed: sac T=1 p95 −8.82%, soroswap T=1 p95 −8.46%
   - All gains were within run-to-run variance

3. **Root cause confirmed: allocation overhead is not the bottleneck.** Per-CxxBuf cost is ~300-500ns (dominated by XDR serialization, not the allocation). Total CxxBuf overhead per SAC transfer is ~3.3-5.5µs against ~50-200µs per-tx apply time (2-7%). Even eliminating all allocation overhead via batching only saves ~1.5µs per tx — well below noise.

### Lesson Learned

This hypothesis was already fully investigated through the complete pipeline including independent benchmarking. The `CxxBatchBuf` mechanism was built, tested, and rejected because per-entry allocation overhead (~300-500ns/CxxBuf) is dominated by the unavoidable XDR serialization cost, and total bridge allocation overhead is only 2-7% of per-tx time. See also soroban-env fail summary meta-pattern #3: "Per-TX unique vs. ledger-shared data" — caching or batching per-TX-unique data is ceiling-bounded at <0.5% of ledger close.
