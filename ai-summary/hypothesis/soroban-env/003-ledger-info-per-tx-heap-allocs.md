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
