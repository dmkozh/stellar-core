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

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (success #001 cached deserialization; fail #005 investigated sharing cache across shallow_clone; fail #007 investigated caching Budget template; this targets the byte-comparison cache-key mechanism specifically)
**Failed At**: reviewer

### Trace Summary

Traced the complete cache hit path from `invoke_host_function_or_maybe_panic()` (soroban_proto_any.rs:414–418) through `get_or_deserialize_cost_params()` (soroban_proto_any.rs:797–816). After success #001's optimization, the cache hit path consists of: (1) `RwLock::read()` acquisition on a non-contended per-thread lock (~20–50ns), (2) `memcmp` of ~600-byte serialized cost param buffers (~5–10ns with SIMD), (3) `ContractCostParams.clone()` of the cached value (~100–200ns). The proposal eliminates only steps 1–2 by replacing with an epoch check. The `clone()` in step 3 — which is 2–5× more expensive than steps 1–2 combined — remains regardless.

### Code Paths Examined

- `src/rust/src/soroban_proto_any.rs:797-816` — `get_or_deserialize_cost_params()`: acquires RwLock read guard, compares `cached_bytes.as_slice() == buf.data.as_slice()`, clones cached params on hit. The byte comparison is a single `memcmp` on ~600-byte buffers (~5–10ns on modern CPUs with SIMD optimization).
- `src/rust/src/soroban_proto_any.rs:414-418` — call site in `invoke_host_function_or_maybe_panic()`: calls `get_cpu_cost_params` and `get_mem_cost_params` per TX, 2 cache lookups total.
- `src/rust/src/soroban_proto_any.rs:787-794` — `shallow_clone()`: each thread gets its own `ProtocolSpecificModuleCache` with independent `RwLock`s, so there is zero cross-thread contention on the read locks.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:58-62` — C++ side: `toCxxBuf(cpu)` and `toCxxBuf(mem)` serialize cost params via `xdr_to_opaque()` per TX; this C++ serialization cost (~0.5–1μs per TX) dwarfs the Rust-side byte comparison and is not addressed by the epoch-key proposal.

### Why It Failed

The targeted overhead is too small to produce a measurable benchmark improvement:

1. **The byte comparison is trivially cheap.** Two `memcmp` calls on ~600-byte buffers cost ~10–20ns total per TX. Modern x86-64 CPUs handle this with a single SIMD comparison pass. Even adding the non-contended `RwLock::read()` cost (~40–100ns), the total targeted overhead is ~50–120ns per TX.

2. **Aggregate impact is 0.04–0.09% of baseline.** For SAC T=1: 6400 TXs × ~120ns = ~768μs / 850ms ≈ 0.09%. For SAC T=8: 800 TXs/thread × ~120ns = ~96μs / 700ms ≈ 0.01%. All scenarios are 50–100× below the minimum 5% Low severity threshold.

3. **The dominant cache-hit cost is untouched.** `ContractCostParams.clone()` at ~100–200ns per param (×2) is 2–5× more expensive than the byte comparison + lock combined. And `Budget::try_from_configs()` which consumes the cloned params is more expensive still (fail #007 estimated ~1–3μs per TX). An epoch key saves ~50–120ns out of a ~2–6μs cache-hit-to-budget-construction path — a ~2–5% reduction in an already-small overhead.

4. **The C++ serialization cost is larger and unaddressed.** `toCxxBuf(cpu)` and `toCxxBuf(mem)` in `getLedgerInfo()` (InvokeHostFunctionOpFrame.cpp:61–62) still serialize the cost params via `xdr_to_opaque()` per TX (~0.5–1μs total). This is 5–10× the cost of the Rust-side byte comparison. The epoch-key approach doesn't eliminate this C++ serialization.

### Lesson Learned

After success #001 eliminated the expensive per-TX XDR *deserialization*, the remaining cache-hit overhead is dominated by `ContractCostParams.clone()` and `Budget::try_from_configs()`, not by the cache lookup mechanism. Optimizing the cache key from byte-comparison to epoch-comparison saves ~50–120ns per TX — orders of magnitude below benchmark noise. Further cost-param path improvements require either eliminating the clone (sharing immutable params via `Arc`) or caching the Budget template (fail #007), both of which require soroban-env-host API changes that are out of scope.
