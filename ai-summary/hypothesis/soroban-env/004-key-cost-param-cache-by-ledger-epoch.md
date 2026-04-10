# H004: Key the Cost-Param Cache by Ledger Epoch Instead of Byte-Comparing on Every Transaction

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: CPU / bridge cache lookup
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Within a ledger close, the bridge should treat cost parameters as ledger-scoped
state and hit the Rust-side cache using a small epoch key (ledger sequence /
config epoch), not by re-checking the full serialized cost-param bytes on every
transaction.

## Mechanism

`invoke_host_function_or_maybe_panic()` fetches cost params on every invocation
through `get_cpu_cost_params()` / `get_mem_cost_params()`. The cache hit path in
`get_or_deserialize_cost_params()` acquires a read lock, compares the cached
serialized bytes against the current `CxxBuf` with `cached_bytes.as_slice() ==
buf.data.as_slice()`, and only then clones the cached `ContractCostParams`.

But `CxxLedgerInfo` already carries the ledger sequence, and `getLedgerInfo()`
always populates cost params from the ledger-scoped `SorobanNetworkConfig`. In
the apply-load benchmark, all transactions in the close share that same config,
so the per-tx byte comparison is a residual bridge cost that could be replaced
with a cheap epoch check plus cached params.

## Trigger

Run any apply-load scenario after the existing cost-param deserialization cache
is warm. Every subsequent transaction still pays two `RwLock` reads and two
bytewise equality checks on the serialized CPU / memory cost-param buffers.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:41-69` — `getLedgerInfo()` rebuilds `CxxLedgerInfo` per tx from ledger-scoped config + sequence number
- `src/rust/src/soroban_proto_any.rs:410-430` — every invocation fetches CPU / mem cost params before building `Budget`
- `src/rust/src/soroban_proto_any.rs:797-816` — cache hit path byte-compares serialized buffers on every tx

## Evidence

The bridge already knows enough to key the cache more cheaply: the ledger
sequence is passed in `CxxLedgerInfo`, and `getLedgerInfo()` sources both cost
params from the current `SorobanNetworkConfig`. After the first transaction in a
close, the remaining transactions do not need the bytewise "did config change?"
check unless the bridge expects the config to mutate mid-ledger, which the apply
path does not.

## Anti-Evidence

This is a residual optimization after the more important cost-param work already
captured in reviewed findings. `ContractCostParams.clone()` and
`Budget::try_from_configs()` still remain, so skipping the byte comparison may
end up below the benchmark noise floor unless it is paired with other cost-param
path simplifications.
