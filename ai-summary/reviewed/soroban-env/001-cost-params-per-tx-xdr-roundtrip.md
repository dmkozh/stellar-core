# H001: Redundant ContractCostParams XDR Serialization/Deserialization Per Transaction

**Date**: 2026-04-08
**Subsystem**: soroban-env (C++↔Rust bridge)
**Severity**: Medium
**Impact**: 10–20% reduction in per-TX bridge overhead for simple Soroban scenarios (SAC transfers)
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

`ContractCostParams` (CPU and memory cost model parameters) are network-wide
configuration that does not change between transactions within a single ledger
close. When processing N transactions in a ledger, the cost params should be
serialized at most once (or zero times if cached from a previous ledger with
unchanged params), and deserialized at most once on the Rust side.

## Mechanism

Every call to `invoke_host_function` triggers the following redundant work:

1. **C++ side** (`getLedgerInfo` in `InvokeHostFunctionOpFrame.cpp:58-62`):
   - `auto cpu = sorobanConfig.cpuCostParams()` — **copies** the entire
     `ContractCostParams` struct (~86 entries × 20 bytes = ~1720 bytes) because
     `cpuCostParams()` returns `const&` but `auto` deduces by value.
   - `auto mem = sorobanConfig.memCostParams()` — same copy for mem params.
   - `toCxxBuf(cpu)` — XDR-serializes the copy into a new `vector<uint8_t>`
     (~1724 bytes), wrapped in `unique_ptr`.
   - `toCxxBuf(mem)` — same for mem params.

2. **Rust side** (`invoke_host_function_or_maybe_panic` in
   `soroban_proto_any.rs:418-419`):
   - `non_metered_xdr_from_cxx_buf::<ContractCostParams>(&ledger_info.cpu_cost_params)`
     — XDR-deserializes ~1724 bytes back into `ContractCostParams`.
   - Same for `mem_cost_params`.

3. These deserialized params are passed to `Budget::try_from_configs` which
   processes all 86 cost entries to build internal cost models.

**Per-transaction overhead**: 2 C++ struct copies (~3.4KB), 2 XDR serializations
(~3.4KB output), 4 heap allocations, 2 XDR deserializations (~3.4KB input),
plus Budget cost model construction from identical data.

For a ledger close with 100 transactions across 8 threads, this is ~340KB of
redundant XDR processing and hundreds of unnecessary heap allocations.

## Trigger

Run the apply-load benchmark with any Soroban scenario (SAC transfers are most
affected due to short host execution time making bridge overhead proportionally
larger). Observe that `ContractCostParams` serialization and deserialization
appears in per-TX profiles.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:58-62` — C++ side: copies and serializes cost params per TX
- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:412-419` — Rust side: deserializes cost params per TX
- `src/rust/src/bridge.rs:CxxLedgerInfo:80-81` — Bridge type carrying cost params as CxxBuf
- `src/rust/src/soroban_module_cache.rs:SorobanModuleCache` — Existing per-protocol cache that could store cost params

## Evidence

1. `cpuCostParams()` returns `ContractCostParams const&` but `auto cpu = ...`
   deduces `ContractCostParams` (by value), confirmed at line 58-59. This is a
   redundant deep copy of a vector with 86 elements.

2. `toCxxBuf(cpu)` calls `xdr::xdr_to_opaque(t)` which performs full XDR
   serialization into a newly allocated `vector<uint8_t>`.

3. On the Rust side, `non_metered_xdr_from_cxx_buf` performs full XDR
   deserialization with depth/length limit checking per field.

4. `SorobanNetworkConfig::cpuCostParams()` (NetworkConfig.cpp:2409) simply
   returns `mCpuCostParams` which is set once during config loading and doesn't
   change between transactions.

5. The `SorobanModuleCache` already exists as a shared object across
   invocations, demonstrating the pattern of caching per-ledger data on the
   Rust side.

## Anti-Evidence

1. The per-TX overhead of ~15-25μs for cost params round-trip may be small
   relative to host execution time for complex contracts (soroswap: ~500μs+).
   Impact is most visible for simple SAC transfers where host time is ~50-100μs.

2. Caching on the Rust side would require either adding fields to
   `SorobanModuleCache` or introducing a new per-ledger cache object, both
   requiring bridge API changes.

3. The `Budget::try_from_configs` call processes the cost params internally
   (inside soroban-env-host), so even with cached deserialized params, the
   budget construction cost remains unless the host API changes.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full per-TX path from C++ `getLedgerInfo()` through `toCxxBuf` serialization, across the FFI boundary into Rust `invoke_host_function_or_maybe_panic`, where `non_metered_xdr_from_cxx_buf` deserializes the same constant data. Confirmed `cpuCostParams()` returns `ContractCostParams const&` (NetworkConfig.h:430, NetworkConfig.cpp:2409) but `auto cpu = ...` at InvokeHostFunctionOpFrame.cpp:58 copies by value. Confirmed this path executes per-TX via `invokeHostFunction()` at line 544-553 which calls `getLedgerInfo()` at line 551. Both the pre-v23 and parallel apply helpers call the same `stellar::getLedgerInfo()` free function per TX (lines 979, 1164).

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:42-70` — Confirmed: constructs CxxLedgerInfo per-TX, copies cost params by value (line 58-59), XDR-serializes via `toCxxBuf` (line 61-62). `toCxxBuf` (TransactionUtils.h:372-376) calls `xdr::xdr_to_opaque(t)` which allocates a new `vector<uint8_t>` and serializes.
- `src/ledger/NetworkConfig.h:430-431` — Confirmed: `cpuCostParams()` and `memCostParams()` return `ContractCostParams const&`.
- `src/ledger/NetworkConfig.cpp:2409-2418` — Confirmed: simply returns `mCpuCostParams`/`mMemCostParams` member references; data is constant per ledger.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:invokeHostFunction:526-553` — Confirmed: calls `getLedgerInfo()` at line 551 as part of every `invoke_host_function` bridge call.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:975-982,1161-1168` — Both pre-v23 and parallel apply helpers override `getLedgerInfo()` by calling the same free function.
- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:391-420` — Confirmed: deserializes cost params from CxxBuf at lines 418-419 via `non_metered_xdr_from_cxx_buf`, passes to `Budget::try_from_configs` at line 412.
- `src/rust/src/soroban_proto_any.rs:non_metered_xdr_from_cxx_buf:136-147` — Confirmed: full XDR deserialization with `read_xdr` and `Limited` depth/length checking.
- `src/rust/src/bridge.rs:CxxLedgerInfo:70-81` — Confirmed: `cpu_cost_params` and `mem_cost_params` are `CxxBuf` fields on the per-TX ledger info struct.
- `src/rust/src/soroban_module_cache.rs:ProtocolSpecificModuleCache:701-710` — Confirmed: existing shared cache structure that could be extended to hold deserialized cost params.

### Findings

**The inefficiency is confirmed and real.** Every Soroban transaction performs:
1. Two redundant deep copies of `ContractCostParams` vectors (~1720 bytes each) due to `auto` value deduction on C++ const-ref return.
2. Two XDR serializations of ~1720 bytes each into newly allocated vectors.
3. Two XDR deserializations of ~1720 bytes each on the Rust side.
4. Six associated heap allocations (2 copies + 2 CxxBuf vectors + 2 Rust ContractCostParams vectors).

All from data that is constant across all transactions in a ledger.

**Severity downgrade from Medium to Low.** The hypothesis claims 10-20% improvement on SAC transfer scenarios. My analysis of the actual data sizes and operations involved suggests the per-TX cost is ~3-8μs (not the 15-25μs estimated):
- 2 C++ deep copies of ~1720-byte vectors: ~0.5-1μs each
- 2 XDR serializations of ~1720 bytes (essentially memcpy with byte-swapping): ~0.5-1.5μs each
- 2 Rust XDR deserializations: ~1-2μs each
- Heap allocations: ~0.5-1μs total

For SAC transfers at ~100-150μs total TX time, 3-8μs savings represents ~2-7% improvement. This is at the Low severity threshold (5-10%), not Medium.

**Key constraint:** `Budget::try_from_configs` (inside soroban-env-host, a black box per scope) takes `ContractCostParams` by value and must be called per-TX since `instruction_limit` varies per transaction. Even with Rust-side caching of deserialized params, cloning them for each Budget construction is still required (~1-2μs), partially offsetting the deserialization savings.

**Existing optimizations:** None found for this specific path. The `SorobanModuleCache` caches parsed Wasm modules but not cost parameters.

### PoC Guidance

- **Target code**:
  - `src/transactions/InvokeHostFunctionOpFrame.cpp:getLedgerInfo:58-62` — Change `auto cpu = ...` to `auto const& cpu = ...` (eliminates copy). Optionally, cache the serialized `CxxBuf` objects per-ledger in the apply helper classes.
  - `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:412-419` — Cache deserialized `ContractCostParams` in `ProtocolSpecificModuleCache` (field addition + populate on first use). Clone from cache instead of deserializing from XDR each time.
- **Change description**: Eliminate redundant per-TX XDR serialization/deserialization of constant `ContractCostParams`. C++ side: avoid value-copy of const-ref return and cache serialized CxxBuf. Rust side: cache deserialized params in module cache, clone for `Budget::try_from_configs`.
- **Correctness check**: Existing test suite should cover this — the data passed to `Budget::try_from_configs` must remain identical. Run `[soroban]` tag tests and any SAC-specific tests.
- **Benchmark focus**: apply-load benchmark with SAC transfer scenario. Measure per-TX bridge overhead (time from `invokeHostFunction` entry to `invoke_host_function_or_maybe_panic` Budget construction). Expect ~2-7% improvement in total per-TX time for simple SAC scenarios. Profile with tracy spans to isolate the cost params path.
