# H001: Batch Rust Bridge Outputs For Events And Ledger Effects

**Date**: 2026-04-10
**Subsystem**: crypto, rust
**Severity**: Medium
**Impact**: FFI allocation and copy overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The Soroban apply path should return collections of modified ledger entries and
contract events across the C++↔Rust bridge in one contiguous payload per
collection, not as one independently allocated buffer per item. Write-heavy
benchmark transactions should spend their time in host execution and ledger
application, not in repeated small Rust `Vec<u8>` allocations and cxx.rs vector
marshaling for every returned event and effect.

## Mechanism

`InvokeHostFunctionOutput` still exposes `contract_events` and
`modified_ledger_entries` as `Vec<RustBuf>`, and Rust fills those vectors by
allocating one `Vec<u8>` per encoded entry/event. C++ then immediately walks
those vectors and decodes each buffer one-by-one, so `custom_token` and
`soroswap` workloads pay O(N) bridge allocations twice per tx on the measured
apply path even though the input side already solved the same problem with
`CxxBatchBuf`.

## Trigger

Run `custom_token` or `soroswap` apply-load, especially `T=8`, and sample
allocator / cxx bridge traffic during `extract_ledger_effects`,
`encoded_contract_events` collection, `recordStorageChanges`, and
`collectEvents`. Compare against a build that returns batched output buffers
(`data + lengths`) for contract events and modified ledger entries.

## Target Code

- `src/rust/src/bridge.rs:43-63` — `InvokeHostFunctionOutput` returns `Vec<RustBuf>` collections
- `src/rust/src/soroban_proto_any.rs:261-301` — `extract_ledger_effects` builds one `RustBuf` per modified entry
- `src/rust/src/soroban_proto_any.rs:497-514` — successful host invocation maps encoded contract events into `Vec<RustBuf>`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:667-716` — C++ decodes each modified entry one-at-a-time in `recordStorageChanges`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:763-793` — C++ decodes each contract event one-at-a-time in `collectEvents`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:42-92` — input side already uses `CxxBatchBufBuilder` specifically to avoid per-entry bridge allocations

## Evidence

The bridge already contains an explicit batched input format because individual
per-entry buffers were expensive enough to justify `CxxBatchBuf`. The output
path is asymmetric: Rust still emits vectors of individual buffers for both
modified entries and contract events, and C++ immediately consumes them in a
tight per-item loop.

## Anti-Evidence

`sac` transfers modify fewer entries and emit fewer events than `custom_token`
or `soroswap`, so the benefit should be concentrated in the heavier scenarios.
`result_value` is still a single buffer and would remain on the old path unless
the bridge format is widened further.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — substantially duplicates soroban-env fail #017 ("Return all modified output entries as single contiguous buffer with offset table")
**Failed At**: reviewer

### Trace Summary

Traced the complete output path from soroban-env-host through the bridge layer to C++ consumption. The hypothesis claims "allocating one `Vec<u8>` per encoded entry/event" but this is incorrect: `extract_ledger_effects` moves pre-encoded `Vec<u8>` from `LedgerEntryChange.encoded_new_value` into `RustBuf` via `From<Vec<u8>>`, which is a zero-cost struct wrapping (moves 3 pointer-sized fields, no heap allocation). Contract events follow the same pattern — `res.encoded_contract_events` is `Vec<Vec<u8>>` from the host crate, and `RustBuf::from(Vec<u8>)` just wraps each existing allocation. The only actual bridge-layer allocation is the outer `Vec<RustBuf>` container and a few small TTL entry serializations (~50 bytes each).

### Code Paths Examined

- `src/rust/src/common.rs:3-7` — `impl From<Vec<u8>> for RustBuf` is a zero-cost struct wrap: `Self { data: value }`. No heap allocation occurs.
- `src/rust/src/soroban_proto_any.rs:268-271` — `encoded_new_value.into()` moves the host-allocated `Vec<u8>` into `RustBuf`. Zero-cost move.
- `src/rust/src/soroban_proto_any.rs:510-514` — `res.encoded_contract_events.into_iter().map(RustBuf::from).collect()` consumes the host's `Vec<Vec<u8>>` and wraps each inner vec. Zero-cost moves per item; only the outer `Vec<RustBuf>` is newly allocated.
- `src/rust/src/soroban_proto_any.rs:294-296` — TTL entries are the only items where `non_metered_xdr_to_rust_buf` creates a new allocation (~50 bytes per TTL entry).
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:58` — `encoded_contract_events: Vec<Vec<u8>>` — events are already encoded inside the host crate (out of scope). The bridge just wraps them.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:673-676` — C++ accesses `buf.data` as `rust::Vec<uint8_t>` which directly reads Rust-allocated memory through cxx.rs's wrapper. No additional copy occurs for the iteration itself.

### Why It Failed

1. **The claimed inefficiency does not exist in the bridge layer.** The per-entry `RustBuf` wrapping is a zero-cost move (`From<Vec<u8>>` just wraps the existing allocation), not a new heap allocation. The actual `Vec<u8>` allocations for encoded entries and events happen inside `soroban-env-host` (out of scope).

2. **Substantially duplicates soroban-env fail #017**, which investigated the identical proposal (returning modified output entries as a single contiguous buffer with offset table). That investigation concluded: per-entry overhead is ~80 ns/TX × 5 entries = 400 ns/TX total, ~0.3% of baseline. Batch-buffer FFI only helps when per-item overhead is >1 µs or item counts are >1000/TX. Standard benchmark scenarios have 3–10 modified entries and 1–5 events per TX — far below the threshold.

3. **The asymmetry with `CxxBatchBuf` on the input side is misleading.** The input side benefits from batching because C++ must serialize per-entry XDR into new buffers (the data doesn't already exist as encoded bytes). On the output side, the host crate already provides pre-encoded `Vec<u8>` for each item, so the bridge just wraps and moves them — the allocation cost that `CxxBatchBuf` avoids on input simply doesn't exist on output.

### Lesson Learned

When a hypothesis claims per-entry allocation overhead across the FFI boundary, trace whether `RustBuf::from(Vec<u8>)` actually allocates or just moves an existing allocation. On the output path, entries and events arrive pre-encoded from `soroban-env-host` as `Vec<u8>`, and the `From` impl is a zero-cost move. The real allocation work is inside the host crate (out of scope). Only the small TTL entry serialization and the outer container vectors are bridge-layer allocations.
