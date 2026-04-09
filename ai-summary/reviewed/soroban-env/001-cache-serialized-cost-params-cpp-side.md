# H001: Cache Serialized Cost Params Bytes on C++ Side

**Date**: 2025-07-22
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: Eliminate redundant per-TX XDR serialization of cost params
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

The `getLedgerInfo()` function should serialize `ContractCostParams` once per
ledger close (or once per thread in parallel apply), not once per transaction.
Since `cpuCostParams()` and `memCostParams()` return `const&` references to
network config values that are immutable within a ledger close, repeated
serialization produces identical bytes every time.

## Mechanism

`getLedgerInfo()` (InvokeHostFunctionOpFrame.cpp:42-70) calls `toCxxBuf(cpu)`
and `toCxxBuf(mem)` on every transaction invocation. Each call performs XDR
serialization (`xdr_to_opaque`) of a `ContractCostParams` (~28 entries,
~300-600 bytes serialized) into a newly heap-allocated `vector<uint8_t>`,
wrapped in `unique_ptr`. This produces identical bytes for every TX in the
same ledger because the cost params are ledger-scoped network config.

Success finding #001 already caches the *deserialized* cost params on the Rust
side (avoiding per-TX XDR round-trip through the bridge), but the C++ side
still performs the serialization unconditionally. With the Rust cache, these
serialized bytes are only used for a `memcmp` cache-validity check on the Rust
side — they are never deserialized after the first TX.

The per-TX cost is ~500-1100ns (two XDR serializations + two heap allocations).
For 6400 SAC TXs, this totals ~3.2-7ms. Against a ~850ms T=1 baseline, this
is ~0.4-0.8% — below the 5% Low severity threshold but a clean follow-up to
success #001 that eliminates the last redundancy in the cost params path.

## Trigger

Run the apply-load benchmark with any scenario. Every TX invocation calls
`getLedgerInfo()` which serializes cost params.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:58-62` — `toCxxBuf(cpu)` and `toCxxBuf(mem)` in `getLedgerInfo()`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:975-982` — `SingleApply::getLedgerInfo()` calls `stellar::getLedgerInfo()` per TX
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1162-1167` — `ParallelApply::getLedgerInfo()` calls same per TX

## Evidence

- `getLedgerInfo()` is called once per TX (line 551 in `invokeHostFunction`)
- `sorobanConfig.cpuCostParams()` returns `const&` — value is immutable per ledger
- `toCxxBuf` allocates `make_unique<vector<uint8_t>>()` + `xdr_to_opaque()` each time
- Rust side only uses the bytes for `memcmp` after first TX (soroban_proto_any.rs:797-831)

## Anti-Evidence

- The per-TX saving (~500-1100ns) is small relative to total TX time (~95-133μs)
- The optimization is below the benchmark noise floor (~1-2% variance)
- CxxLedgerInfo is a move-only struct that can't trivially share pre-serialized fields across calls without interface changes
- Similar micro-optimizations (fail #003 network_id, fail #004 trace log) were already rejected at final review as below noise floor

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated. Success #001 cached deserialized params on the Rust side; this targets the residual C++ serialization. Fail #003 noted cost params serialization as "covered by H001" but that referred to the Rust-side deserialization cache (now success #001), not C++ serialization caching.

### Trace Summary

Traced the per-TX path from `getLedgerInfo()` (InvokeHostFunctionOpFrame.cpp:42-69) where `toCxxBuf(cpu)` and `toCxxBuf(mem)` each call `xdr_to_opaque()` + `make_unique<vector<uint8_t>>()`, through the CXX bridge into `get_or_deserialize_cost_params()` (soroban_proto_any.rs:797-816) where the serialized bytes are compared via `memcmp` against the cached copy. After success #001's Rust-side cache, the serialized bytes are never deserialized after the first TX — they serve only as cache-validity keys. The C++ serialization is therefore pure redundant work after the first TX in a ledger close.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:42-69` — Confirmed: lines 58-62 bind cost params by `const&` (success #001 already changed from copy to `const&`), then `toCxxBuf(cpu)` and `toCxxBuf(mem)` each call `xdr_to_opaque()` producing ~300-600 bytes + `make_unique<vector<uint8_t>>()` heap allocation.
- `src/transactions/TransactionUtils.h:toCxxBuf:370-376` — Confirmed: `toCxxBuf(T const& t)` calls `xdr::xdr_to_opaque(t)` which serializes the entire `ContractCostParams` XDR struct into a new `vector<uint8_t>`.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:551` — Confirmed: `getLedgerInfo()` called per-TX via `invokeHostFunction`.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:975-982` — Confirmed: pre-v23 helper calls `stellar::getLedgerInfo()` per TX.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1161-1168` — Confirmed: parallel apply helper calls `stellar::getLedgerInfo()` per TX.
- `src/rust/src/soroban_proto_any.rs:414-424` — Confirmed: `get_cpu_cost_params(&ledger_info.cpu_cost_params)` and `get_mem_cost_params(&ledger_info.mem_cost_params)` use the cache on p23+.
- `src/rust/src/soroban_proto_any.rs:797-816` — Confirmed: `get_or_deserialize_cost_params` fast path acquires read lock, compares `cached_bytes.as_slice() == buf.data.as_slice()` (~600-1200 byte memcmp). On cache hit, returns `cached_params.clone()`. On miss (first TX only), deserializes and updates cache.
- `src/ledger/NetworkConfig.cpp:2409-2416` — Confirmed: `cpuCostParams()` and `memCostParams()` return `const&` to member variables immutable within a ledger close.

### Findings

**The inefficiency is confirmed but the practical impact is very small.**

The C++ side serializes `ContractCostParams` twice per TX via `xdr_to_opaque()`, producing identical bytes every time within a ledger close. With success #001's Rust-side cache, these bytes are only used for a `memcmp` cache-validity check — never deserialized after the first TX.

**Per-TX cost breakdown:**
1. `xdr_to_opaque(cpuCostParams)` — serializes ~28 `ContractCostParamEntry` structs (~300-600 bytes): ~200-500ns
2. `xdr_to_opaque(memCostParams)` — same: ~200-500ns
3. Two `make_unique<vector<uint8_t>>()` heap allocations: ~60-100ns total
4. Total per-TX: ~460-1100ns

**Proposed fix approach:**
Cache the `vector<uint8_t>` results in the `SingleApply` / `ParallelApply` helper classes (one cache per thread). Construct `CxxBuf` by copying the cached bytes instead of re-serializing. This would replace `xdr_to_opaque` (~400-1000ns) with `memcpy` of ~600-1200 bytes (~50-100ns), saving ~350-900ns per TX. The heap allocation for the `CxxBuf` wrapper is still required since `CxxBuf` owns a `UniquePtr<CxxVector<u8>>`.

**Impact estimate:**
- Net saving per TX: ~350-900ns
- For 6400 SAC TXs: ~2.2-5.8ms
- Against ~750ms T=1 baseline (post success #001): ~0.3-0.8%
- This is below the benchmark noise floor (~1-2% variance)

**The hypothesis's severity self-assessment of Informational is accurate.** The finding follows the same pattern as fail #003 (network_id clone, ~0.02-0.05%, rejected at final review) and fail #004 (trace log check, <1%, rejected at final review). The optimization is correct but the impact is too small for measurable benchmark improvement.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp` — In both `InvokeHostFunctionPreV23ApplyHelper` (line ~975) and `InvokeHostFunctionParallelApplyHelper` (line ~1162), cache the serialized cost params bytes as member variables (`std::vector<uint8_t> mCachedCpuCostBytes`, `std::vector<uint8_t> mCachedMemCostBytes`) and corresponding `CxxBuf`-constructing helpers. On first call to `getLedgerInfo()`, serialize and cache. On subsequent calls, copy from cache.
- **Change description**: Replace per-TX `xdr_to_opaque(cpuCostParams)` and `xdr_to_opaque(memCostParams)` with cached byte copies. Since `CxxBuf` requires owning a `UniquePtr<CxxVector<u8>>`, the cache stores raw `vector<uint8_t>` bytes and each call constructs a new `CxxBuf` via `make_unique<vector<uint8_t>>(cached_bytes)` (copy, not serialize).
- **Correctness check**: Run `[soroban]` tagged tests — the Rust-side cache validity check depends on receiving the same serialized bytes each time, which copy preserves. Also verify `[tx]` tagged tests for broader coverage.
- **Benchmark focus**: Per-TX bridge setup time in apply-load benchmark. Expected improvement is ~0.3-0.8% — likely below the noise floor. The optimization eliminates real redundant work but is not expected to produce a measurable benchmark improvement.
