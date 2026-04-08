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
