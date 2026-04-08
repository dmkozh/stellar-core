# H003: Per-Transaction CxxLedgerInfo Reconstruction With Redundant Heap Allocations

**Date**: 2026-04-08
**Subsystem**: soroban-env (C++↔Rust bridge)
**Severity**: Low
**Impact**: 5–10% reduction in per-TX bridge overhead; eliminates 3+ heap allocations per invocation
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

For transactions within the same ledger close, the `CxxLedgerInfo` struct
contains fields that are identical across all transactions (protocol_version,
sequence_number, timestamp, network_id, base_reserve, memory_limit, TTL
settings, and cost params). Only the per-transaction Budget limits vary. The
invariant portion of `CxxLedgerInfo` should be constructed once per ledger
close and reused, with only the varying fields set per transaction.

## Mechanism

Every call to `invokeHostFunction` (InvokeHostFunctionOpFrame.cpp:526)
triggers `getLedgerInfo()` which constructs a fresh `CxxLedgerInfo` struct.
This involves:

1. **network_id**: `info.network_id.reserve(networkID.size())` followed by a
   byte-by-byte loop (line 64-68) that calls `emplace_back` 32 times on a
   `rust::Vec<uint8_t>`. The `rust::Vec` API does not support bulk insertion,
   so each byte is pushed individually. The initial `reserve(32)` allocates
   the rust Vec's internal buffer (1 heap allocation), then 32 individual
   push_back calls follow.

2. **cost params**: Two `toCxxBuf()` calls (lines 61-62) each allocate a
   `unique_ptr<vector<uint8_t>>` and perform XDR serialization (2 heap
   allocations + serialization work). This is the dominant cost, detailed in
   H001.

3. **LedgerInfo conversion on Rust side** (soroban_proto_any.rs:63-78):
   `c.network_id.clone()` clones the 32-byte `Vec<u8>` (1 heap allocation),
   then `.try_into()` converts to `[u8; 32]` and the cloned Vec is dropped.
   Using `c.network_id.as_slice().try_into()` would avoid this allocation.

**Total per-TX heap allocations from CxxLedgerInfo alone**: At least 4 (2 for
cost param CxxBufs, 1 for network_id rust::Vec, 1 for network_id clone in
Rust). For 100 TXs across 8 threads = 3200+ unnecessary heap allocations per
ledger close.

## Trigger

Run the apply-load benchmark with SAC transfer scenario. Profile heap
allocations in `getLedgerInfo` and `TryFrom<&CxxLedgerInfo> for LedgerInfo`.
Each will show per-TX allocation patterns.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:41-69` — Per-TX construction of CxxLedgerInfo
- `src/transactions/InvokeHostFunctionOpFrame.cpp:64-68` — Byte-by-byte network_id copy loop
- `src/rust/src/soroban_proto_any.rs:63-78` — Rust-side LedgerInfo conversion with redundant Vec clone
- `src/rust/src/soroban_proto_any.rs:70` — `c.network_id.clone().try_into()` unnecessary clone

## Evidence

1. `getLedgerInfo` is called once per transaction invocation, confirmed by
   `InvokeHostFunctionApplyHelper::invokeHostFunction` (line 551) and both
   the pre-v23 (line 975) and parallel (line 1162) getLedgerInfo overrides.

2. The byte-by-byte network_id copy loop (line 65-68) uses `emplace_back`
   instead of bulk copy. The 32-byte Hash is always the same for all TXs
   in a ledger.

3. On the Rust side (soroban_proto_any.rs:70), `.clone()` on a `Vec<u8>` of
   length 32 allocates a new 32-byte Vec, copies the data, then `try_into()`
   produces `[u8; 32]` from the clone. A direct `as_slice().try_into()` would
   achieve the same result without allocation.

4. All scalar fields (protocol_version, sequence_number, etc.) are also
   identical across TXs in a ledger — they come from the ledger header and
   soroban config, neither of which changes during a ledger close.

## Anti-Evidence

1. Individual allocations are small (32-1724 bytes). Modern allocators
   (jemalloc/tcmalloc) handle small allocations very efficiently, often from
   thread-local caches with no syscalls.

2. The `CxxLedgerInfo` struct is passed by value across the FFI. Caching it
   would require either passing by reference (not supported well by CXX) or
   restructuring the bridge to separate per-ledger from per-TX data.

3. The network_id Vec clone on Rust side is only 32 bytes — the allocation
   overhead (likely 64 bytes including allocator metadata) is small in absolute
   terms.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated in fail/ or success/

### Trace Summary

Traced the full per-TX path from C++ `getLedgerInfo()` (InvokeHostFunctionOpFrame.cpp:42-69) through the CXX bridge into Rust `TryFrom<&CxxLedgerInfo> for LedgerInfo` (soroban_proto_any.rs:63-78). Confirmed that `getLedgerInfo()` is called per-TX via both the pre-v23 helper (line 975) and parallel apply helper (line 1162), each delegating to the same free function. The network_id byte-by-byte loop (lines 64-68) fills a `rust::Vec<uint8_t>` with `reserve(32)` + 32 `emplace_back` calls. On the Rust side, `c.network_id.clone().try_into()` at line 70 allocates a new `Vec<u8>`, copies 32 bytes, converts to `[u8; 32]`, then drops the Vec. The cost params serialization (lines 61-62) is the dominant cost but is already covered by the in-flight H001 hypothesis.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:42-69` — Confirmed: constructs CxxLedgerInfo per-TX. Lines 64-68: `reserve(32)` pre-allocates the `rust::Vec`, then 32 `emplace_back` calls fill it byte-by-byte. After `reserve`, each `emplace_back` is a pointer-write + size-increment (no reallocation), so the loop cost is ~32 iterations × (bounds check + byte write) ≈ 50-100ns.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:551` — Confirmed: `getLedgerInfo()` called per-TX as part of `invoke_host_function` bridge call.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:975-982` — Confirmed: pre-v23 helper calls `stellar::getLedgerInfo()` per-TX.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1161-1168` — Confirmed: parallel apply helper calls `stellar::getLedgerInfo()` per-TX.
- `src/rust/src/soroban_proto_any.rs:63-78` — Confirmed: `TryFrom<&CxxLedgerInfo> for LedgerInfo` at line 70 does `c.network_id.clone().try_into()`. The `.clone()` on `Vec<u8>` allocates a new 32-byte Vec (heap alloc + memcpy). The `.try_into()` copies 32 bytes into `[u8; 32]` on the stack. The cloned Vec is then dropped (heap dealloc). Using `c.network_id.as_slice().try_into()` would produce the same `[u8; 32]` via a direct 32-byte stack copy from the slice, avoiding the allocation entirely.
- `src/rust/src/bridge.rs:70-82` — Confirmed: `CxxLedgerInfo` shared struct with `network_id: Vec<u8>`. In CXX shared structs, `Vec<u8>` maps to standard Rust `Vec<u8>`, which implements `Deref<Target=[u8]>`, so `.as_slice()` is available.
- `src/transactions/TransactionUtils.h:372-376` — Confirmed: `toCxxBuf` calls `xdr::xdr_to_opaque(t)` for each cost params serialization (already covered by H001).

### Findings

**The inefficiency is confirmed but the impact is smaller than claimed.**

The hypothesis bundles three categories of waste:

1. **Cost params XDR roundtrip** (lines 58-62 C++, lines 418-419 Rust): This is the dominant per-TX cost (~3-8μs) involving XDR serialization of ~1720 bytes × 2, plus deserialization. **This is already covered by the in-flight H001 hypothesis** (reviewed/soroban-env/001), which correctly identifies this as the main target.

2. **network_id byte-by-byte copy** (C++ lines 64-68): After `reserve(32)`, the 32 `emplace_back` calls are inexpensive — each is just a byte write and size increment with no reallocation. The `reserve` call itself allocates the `rust::Vec` backing storage (one heap allocation of 32 bytes). Total cost: ~50-100ns for the loop, plus one 32-byte heap allocation (~30-50ns via thread-local allocator cache). **Real but negligible.**

3. **Rust-side network_id clone** (soroban_proto_any.rs:70): `c.network_id.clone()` allocates 32 bytes, copies, then the clone is dropped after `try_into()` extracts `[u8; 32]`. Fix: `c.network_id.as_slice().try_into()` — avoids the allocation entirely. **Trivially correct, negligible impact (~30-50ns per TX).**

**Severity downgrade from Low to Informational.** Excluding the cost params (covered by H001), the remaining unique contribution of H003 saves approximately:
- 1 heap allocation (32 bytes) for the Rust-side clone: ~30-50ns
- The network_id loop overhead is not eliminable (rust::Vec doesn't support bulk insert from C++)
- Total per-TX savings: ~30-50ns

At ~100-200μs per SAC transfer TX, this represents ~0.02-0.05% improvement — well below the 5% threshold for Low severity. The finding is real and the fix is correct, but the impact is too small for measurable benchmark improvement.

**The broader "cache invariant CxxLedgerInfo" concept is sound** but faces implementation barriers: `CxxBuf` contains `UniquePtr<CxxVector<u8>>` (bridge.rs:13-15), making `CxxLedgerInfo` non-copyable. Caching would require either restructuring the bridge interface or using shared pointers, which is a more invasive change better addressed as part of H001's caching approach.

### PoC Guidance

- **Target code**: `src/rust/src/soroban_proto_any.rs:70` — Change `c.network_id.clone().try_into()` to `c.network_id.as_slice().try_into()`
- **Change description**: Eliminate one redundant 32-byte heap allocation per Soroban TX by converting the network_id `Vec<u8>` slice directly to `[u8; 32]` instead of cloning the Vec first. The `.as_slice()` returns `&[u8]` (no allocation), and `<[u8; 32]>::try_from(&[u8])` copies 32 bytes directly to the stack.
- **Correctness check**: Run `[soroban]` tag tests — the `LedgerInfo` struct must contain the same network_id bytes. This is a semantics-preserving change (same bytes, same error case).
- **Benchmark focus**: This change alone will not produce measurable benchmark improvement. It is a micro-optimization that eliminates unnecessary work. If combined with H001's cost params caching, the cumulative effect on bridge overhead would be more significant.
