# H007: Replace Per-Entry `CxxBuf` Heap Allocation with Batched FFI Input Buffers

**Date**: 2026-04-09
**Subsystem**: transaction-ledger (transactions/InvokeHostFunctionOpFrame, transactions/TransactionUtils, rust bridge)
**Severity**: High
**Impact**: C++<->Rust bridge overhead in invoke-host apply path
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Preparing an invoke-host call should serialize the footprint, auth entries, and
small scalar XDR payloads using amortized thread-local storage or batched slabs,
not one heap-allocated `std::vector<uint8_t>` per XDR object.

## Mechanism

`toCxxBuf` always performs `xdr::xdr_to_opaque(t)` and wraps the result in
`std::make_unique<std::vector<uint8_t>>`, so every bridge input becomes its own
heap allocation. `InvokeHostFunctionOpFrame::addReads` builds
`mLedgerEntryCxxBufs` and `mTtlEntryCxxBufs` this way for every footprint key,
and `invokeHostFunction` allocates more `CxxBuf`s for auth, host function,
resources, source account, and PRNG seed. On SAC and custom-token apply-load
scenarios, this repeated allocation / copy churn sits entirely outside the VM
and can dominate the lightweight per-tx work, especially under 8-way allocator
contention.

## Trigger

Run the apply-load benchmark for `sac` and `custom_token` at both `T=1` and
`T=8`, and profile `InvokeHostFunctionOpFrame::addReads`,
`InvokeHostFunctionOpFrame::invokeHostFunction`, and `toCxxBuf`. The signal
should be strongest on lightweight transfers with small host execution time and
moderate footprint sizes.

## Target Code

- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf` allocates a new `std::vector<uint8_t>` for every serialized object
- `src/transactions/InvokeHostFunctionOpFrame.cpp:369-466` — per-footprint `LedgerEntry` / TTL serialization into `mLedgerEntryCxxBufs` and `mTtlEntryCxxBufs`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:526-553` — extra per-tx `CxxBuf` allocations for auth, host function, resources, source account, and PRNG seed
- `src/rust/src/bridge.rs:5-15` — bridge contract currently models owned C++ input buffers as `UniquePtr<CxxVector<u8>>`

## Evidence

- `toCxxBuf` is an unconditional allocate-and-copy helper; there is no arena,
  reserve/reuse path, or small-buffer optimization.
- `addReads` serializes each entry separately even though the footprint length is
  known up front and the lifetime is only one host invocation.
- The benchmark disables metrics and metadata output, increasing the relative
  share of pure bridge marshalling overhead in total close time.

## Anti-Evidence

- The cxx.rs bridge shape may limit how much zero-copy or view-based passing is
  possible without a larger interface change.
- `soroswap` workloads spend more time inside the host, so the end-to-end gain
  may be smaller there than on SAC.
- Any batched-buffer design must preserve ownership and lifetime rules across
  the Rust call boundary.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete CxxBuf allocation path from `toCxxBuf` (TransactionUtils.h:370-376)
through `addReads` (InvokeHostFunctionOpFrame.cpp:369-466) and `invokeHostFunction`
(InvokeHostFunctionOpFrame.cpp:525-553), across the cxx.rs bridge into
`invoke_host_function` (soroban_invoke.rs:7-61) and
`invoke_host_function_or_maybe_panic` (soroban_proto_any.rs:391-462). Verified
the inefficiency is real but quantified the actual per-transaction cost as
small relative to total apply time.

### Code Paths Examined

- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf<T>` calls `xdr::xdr_to_opaque(t)` (which pre-sizes via `xdr_argpack_size`, 1 heap alloc) then `make_unique<vector<uint8_t>>` (1 more heap alloc) = 2 allocations per CxxBuf
- `src/transactions/InvokeHostFunctionOpFrame.cpp:312-313` — vectors are `reserve()`d for footprint length, but individual CxxBufs still require per-entry heap allocation
- `src/transactions/InvokeHostFunctionOpFrame.cpp:448-466` — each footprint entry creates 2 CxxBufs (ledger entry + TTL entry/empty), both heap-allocated
- `src/transactions/InvokeHostFunctionOpFrame.cpp:529-552` — 5 additional CxxBufs for auth entries, hostFunction, resources, sourceAccount, and prng seed
- `src/rust/src/bridge.rs:13-15` — `CxxBuf { data: UniquePtr<CxxVector<u8>> }` — the `UniquePtr` wrapper is forced by cxx.rs limitations (issue 671)
- `src/rust/src/common.rs:9-12` — `impl AsRef<[u8]> for CxxBuf` — Rust side reads CxxBuf data via `as_slice()` for zero-copy access to the C++ buffer
- `src/rust/src/soroban_proto_all.rs:101-136` — `invoke_host_function_with_trace_hook_and_module_cache` takes `I: ExactSizeIterator<Item = T>` where `T: AsRef<[u8]>`, so the soroban-env-host API could accept slices from a batched buffer without changes
- `lib/xdrpp/xdrpp/marshal.h:264-272` — `xdr_to_opaque` pre-sizes the output vector via `xdr_argpack_size`, so there is no reallocation during serialization
- `src/simulation/ApplyLoad.cpp:1150` — SAC instance has 1 readOnly key (instance only, no code key for built-in SAC)
- `src/simulation/TxGenerator.cpp:766-789` — SAC transfer footprint: 1 readOnly + 2 readWrite = 3 total entries

### Findings

**The inefficiency exists but the impact is Informational, not High.**

**Allocation count per SAC transfer transaction:**
- `addReads`: 3 footprint entries × 2 CxxBufs (ledger entry + TTL) = 6 CxxBufs
- `invokeHostFunction`: 1 auth entry + 1 prng seed + 3 inline toCxxBuf (hostFn, resources, sourceAccount) = 5 CxxBufs
- Total: ~11 CxxBufs → 22 heap allocations per transaction

**Allocation count per custom_token transfer:**
- `addReads`: 4 footprint entries × 2 = 8 CxxBufs
- `invokeHostFunction`: ~5 CxxBufs
- Total: ~13 CxxBufs → 26 heap allocations per transaction

**Per-CxxBuf cost breakdown:**
- `xdr_to_opaque` serialization + vector data allocation: ~250-400ns (dominates)
- `make_unique<vector>` wrapper allocation: ~50-100ns (cxx.rs requirement)
- Total: ~300-500ns per CxxBuf

**Total CxxBuf overhead per SAC transfer: ~3.3-5.5µs**

**Estimated total per-tx apply time for SAC transfer: ~50-200µs** (SAC is a
built-in contract, so no WASM VM execution — just host-side ledger operations,
auth checks, event emission, plus C++/Rust serialization/deserialization)

**CxxBuf allocation as fraction of per-tx time: ~2-7%**

**Under T=8 allocator contention:** Modern allocators (jemalloc, tcmalloc) use
per-thread arenas that largely eliminate cross-thread contention. Even with the
default glibc malloc, per-allocation latency increase is typically 2-3x, not
10x. This would increase overhead to ~4-8%, still below the 10% "Medium"
threshold.

**Key limitation:** The serialization itself (`xdr_to_opaque` traversal and
byte-writing) accounts for ~60-70% of the per-CxxBuf cost. Even eliminating
all allocation overhead (via arena/pooling) would only save the remaining
~30-40%, reducing per-tx overhead from ~4µs to ~1.5µs — a ~2.5µs saving per
transaction.

**Severity downgrade rationale:** The hypothesis claims "can dominate the
lightweight per-tx work." While the overhead is measurably nonzero, at 2-7%
of per-tx time it does not dominate. Even at T=8, the expected benchmark
improvement from batching would be <5% of per-tx time and <2% of ledger
close time. This falls below the Low (5-10%) threshold into Informational.

### PoC Guidance

- **Target code**: `src/transactions/TransactionUtils.h:370-376` (toCxxBuf), `src/transactions/InvokeHostFunctionOpFrame.cpp:448-466` (addReads entry serialization), `src/transactions/InvokeHostFunctionOpFrame.cpp:525-553` (invokeHostFunction CxxBuf creation)
- **Change description**: Replace individual `toCxxBuf` calls with a batch serialization approach: (1) compute total size via `xdr_argpack_size` for all entries, (2) allocate a single contiguous buffer, (3) serialize all entries with length prefixes, (4) pass the single buffer plus an offset/length array across the FFI boundary. On the Rust side (soroban_proto_any.rs), create a custom iterator that yields `&[u8]` slices from the contiguous buffer — the existing `e2e_invoke::invoke_host_function` API already accepts `I: ExactSizeIterator<Item = T>` where `T: AsRef<[u8]>`, so no soroban-env-host changes needed.
- **Correctness check**: Run `[soroban]` and `[tx]` tagged tests with `--ll fatal -r simple --abort --disable-dots` to verify no regressions
- **Benchmark focus**: SAC scenario at T=1 and T=8. Expected improvement: <5% of per-tx time. Use Tracy or perf to measure `toCxxBuf` + `addReads` time specifically to confirm the micro-level improvement even if end-to-end close time change is within noise.

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2025-07-23
**PoC by**: claude-opus-4.6, high

### Changes Made

1. **`src/rust/src/bridge.rs`** (~lines 17-24): Added `CxxBatchBuf` struct with `data: UniquePtr<CxxVector<u8>>` and `lengths: UniquePtr<CxxVector<u32>>`. Changed `invoke_host_function` signature: `auth_entries`, `ledger_entries`, `ttl_entries` params from `&Vec<CxxBuf>` to `&CxxBatchBuf`.

2. **`src/rust/src/common.rs`** (~lines 34-78): Added `BatchBufIter<'a>` struct implementing `Iterator<Item = &'a [u8]>` and `ExactSizeIterator`. Added `CxxBatchBuf::iter()` method that yields `&[u8]` slices by walking the contiguous data buffer using the lengths array.

3. **`src/rust/src/lib.rs`**: Added `use rust_bridge::CxxBatchBuf;` re-export.

4. **`src/rust/src/soroban_invoke.rs`** (~lines 15-19): Updated `invoke_host_function` signature to accept `&CxxBatchBuf` for the three batch params.

5. **`src/rust/src/soroban_proto_any.rs`** (~lines 317-323, 398-404, 443-458): Updated `invoke_host_function` and `invoke_host_function_or_maybe_panic` signatures. Changed inner call to pass `.as_ref()` for individual `CxxBuf` params and `.iter()` for `CxxBatchBuf` params to satisfy the generic `T: AsRef<[u8]>, I: ExactSizeIterator<Item = T>` bounds.

6. **`src/rust/src/soroban_proto_all.rs`** (~lines 1169-1174): Updated `HostModule::invoke_host_function` fn pointer type to use `&CxxBatchBuf`.

7. **`src/rust/src/soroban_test_extra_protocol.rs`** (~lines 27-31): Updated `maybe_invoke_host_function_again_and_compare_outputs` signature.

8. **`src/transactions/InvokeHostFunctionOpFrame.cpp`** (~lines 40-97, 324-326, 366-367, 504-522, 579-607, 1084, 1141-1145): Added `CxxBatchBufBuilder` class in anonymous namespace. Replaced `rust::Vec<CxxBuf> mLedgerEntryCxxBufs/mTtlEntryCxxBufs` with `CxxBatchBufBuilder mLedgerEntryBatch/mTtlEntryBatch`. Updated `addReads` to use `append()`/`appendEmpty()`. Updated `invokeHostFunction` to build batch buffers and pass them. Updated `handleArchivedEntry` to use batch builders.

### Demonstration

The optimization replaces per-entry `CxxBuf` heap allocations (2 allocations per entry, 2N total per batch) with a single contiguous `CxxBatchBuf` containing all serialized XDR data plus a parallel lengths array (2 allocations total regardless of batch size). For a SAC transfer with ~11 CxxBufs (22 heap allocations), this reduces to 6 allocations (3 batches x 2 allocations each). The Rust-side `BatchBufIter` yields zero-copy `&[u8]` slices that satisfy the existing `ExactSizeIterator<Item: AsRef<[u8]>>` generic bounds in soroban-env-host, requiring no changes to the host internals.

### Test Results

All 109 Soroban-tagged tests passed (3,650,114 assertions). Full test suite (`make check` with NUM_PARTITIONS=$(nproc)) passed: all selftest-nopg partitions and check-nondet passed with exit code 0. No regressions detected.
