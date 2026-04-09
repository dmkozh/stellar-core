# H001: Reuse Prevalidated Soroban Base Fee Across Apply

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Low
**Impact**: serial pre-apply CPU / C++↔Rust fee bridge overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When the same `TransactionFrame` has already passed Soroban validation against
the same protocol version and fee configuration, the later apply path should
reuse the previously computed declared resource-fee result instead of
recomputing it. In the apply-load benchmark, the timed pre-apply phase should
not spend serial main-thread time re-running the same immutable tx-size and
resource-fee math that was already done during the generator's up-front
`checkValid()` pass.

## Mechanism

`ApplyLoad` explicitly prevalidates every generated Soroban tx before the timer
starts, but `commonPreApply` still calls `computePreApplySorobanResourceFee`
again for every tx during ledger close. That function recomputes transaction
size via `getResources(...)->getSize()->xdr::xdr_size(mEnvelope)` and then calls
back through the Rust fee bridge, even though the tx envelope, Soroban
resources, and benchmark fee config are unchanged. For batched SAC txs, where
the envelope carries 100 destination addresses, this duplicates large immutable
work in the serial pre-pass that already limits T=8 scaling.

## Trigger

Run `scripts/run_apply_load_matrix.py`, especially `sac,TX=6400,T=8` with the
default `APPLY_LOAD_BATCH_SAC_COUNT=100`. The profile should show
`computePreApplySorobanResourceFee`, `getResources`, and `getSize` inside the
serial `preParallelApply` path even though the same tx objects were already
validated once in `ApplyLoad`.

## Target Code

- `src/transactions/TransactionFrame.cpp:TransactionFrame::computePreApplySorobanResourceFee:1205-1218` — recomputes declared Soroban fee from immutable tx data
- `src/transactions/TransactionFrame.cpp:TransactionFrame::checkValidWithOptionallyChargedFee:1913-1925` — first fee computation during up-front validation
- `src/transactions/TransactionFrame.cpp:TransactionFrame::commonPreApply:2085-2096` — second fee computation on the timed apply path
- `src/transactions/TransactionFrame.cpp:TransactionFrame::getSize:2633-2636` — uncached `xdr::xdr_size(mEnvelope)` on every fee recomputation
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSacPayments:2136-2148` — prevalidates every generated tx before benchmarking
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateTokenTransfers:2336-2341` — same prevalidation pattern for custom-token transfers
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSoroswapSwaps:3201-3206` — same prevalidation pattern for Soroswap swaps

## Evidence

The benchmark generator's explicit prevalidation pass means the same
`TransactionFrame` objects survive into ledger close with their signatures and
hashes already primed, but there is no analogous cache for declared Soroban fee
computation. `TransactionFrame` memoizes `mContentsHash` / `mFullHash`, yet
`getSize()` remains a fresh `xdr::xdr_size(mEnvelope)` walk and
`computePreApplySorobanResourceFee` always crosses the Rust fee bridge again.

## Anti-Evidence

Outside apply-load, transactions can sit across ledgers or protocol/config
changes, so any cache must be keyed by the effective fee configuration rather
than blindly reused forever. The gain is also concentrated in workloads with
large envelopes or explicit prevalidation reuse; tiny single-transfer txs may
only see a modest improvement.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full path from `ApplyLoad::generateSacPayments` through `checkValid` (prevalidation) and then through `preParallelApply` → `commonPreApply` → `computePreApplySorobanResourceFee` and `commonValid` → `commonValidPreSeqNum` → `checkSorobanResources`. Confirmed that `getSize()` (which calls `xdr::xdr_size(mEnvelope)`) is invoked at least twice per transaction during the apply path alone, and the Rust FFI fee computation is fully repeated despite identical inputs. The `mEnvelope` is immutable after construction, making `getSize()` a pure function of immutable state — a textbook memoization candidate.

### Code Paths Examined

- `src/transactions/TransactionFrame.cpp:2633-2636` (`getSize`) — Unconditionally computes `xdr::xdr_size(mEnvelope)` on every call. No caching. For SAC batch txs with 100 destinations, this walks a multi-KB XDR tree recursively.
- `src/transactions/TransactionFrame.cpp:1204-1219` (`computePreApplySorobanResourceFee`) — Calls `getResources(false, protocolVersion)` which calls `getSize()` [call 1 in apply path], then constructs `CxxTransactionResources` and crosses the Rust FFI via `rust_bridge::compute_transaction_resource_fee`.
- `src/transactions/TransactionFrame.cpp:826-987` (`checkSorobanResources`) — Called from `commonValidPreSeqNum` during `commonValid` inside `commonPreApply`. Calls `getSize()` at line 979 [call 2 in apply path] to check against `txMaxSizeBytes`.
- `src/transactions/TransactionFrame.cpp:2085-2097` (`commonPreApply` fee block) — Calls `computePreApplySorobanResourceFee` which triggers both `getSize()` and Rust FFI. Same computation already performed during `checkValid` prevalidation at line 1923.
- `src/transactions/TransactionFrame.cpp:1913-1925` (`checkValidWithOptionallyChargedFee`) — First fee computation during prevalidation. Produces identical `FeePair` result.
- `src/rust/src/soroban_invoke.rs:63-74` (`compute_transaction_resource_fee`) — Rust side: calls `get_host_module_for_protocol` (version lookup) then pure arithmetic fee computation. Lightweight but non-zero FFI overhead.
- `src/ledger/NetworkConfig.cpp:2849-2869` (`rustBridgeFeeConfiguration`) — Constructs `CxxFeeConfiguration` from getter calls — cheap, just field copies.
- `src/simulation/ApplyLoad.cpp:2136-2149` — Prevalidation loop calls `checkValid` on every generated tx. Same `TransactionFrame` objects (shared_ptr) are later processed by `preParallelApply`.
- `lib/xdrpp/xdrpp/types.h:222-227` (`xdr_size`) — Template-based recursive XDR size computation. For variable-size types (vectors, unions), does runtime traversal proportional to structure size.

### Findings

**The redundancy is confirmed across two dimensions:**

1. **`getSize()` is uncached despite immutable input.** `TransactionFrame` already memoizes `mContentsHash` and `mFullHash` via `mutable` lazy fields, but `getSize()` always recomputes `xdr_size(mEnvelope)`. In the apply path through `preParallelApply`, `getSize()` is called at least twice per tx: once inside `computePreApplySorobanResourceFee` (via `getResources`) and once inside `checkSorobanResources`. Adding a `mutable std::optional<uint32_t> mCachedSize` would follow the existing memoization pattern with zero correctness risk.

2. **The Rust FFI fee computation is repeated with identical inputs.** `computePreApplySorobanResourceFee` is called during both `checkValid` (prevalidation, line 1923) and `commonPreApply` (apply, line 2089). For the same tx against the same `(protocolVersion, sorobanConfig)`, the result is deterministic. The FFI call itself is lightweight arithmetic, but the round-trip through `get_host_module_for_protocol` and the `CxxTransactionResources`/`CxxFeeConfiguration` struct construction adds per-call overhead.

**Impact estimate for SAC TX=6400, T=8:** The serial `preParallelApply` phase was estimated at 200-500ms (per reviewed H001). Each `xdr_size` call on a SAC batch envelope (with 100 destination addresses, ~10-20KB XDR) costs an estimated 2-5µs. Saving 2 redundant `xdr_size` calls + 1 Rust FFI call per tx yields ~8-18µs savings × 6400 txs = ~50-115ms. This represents 10-25% of the serial phase and 3-8% of total T=8 close time.

**The `getSize()` cache is the cleanest optimization.** It requires no keying (envelope is immutable), follows existing memoization patterns in the class, and benefits all call sites — not just the fee computation path. The fee-result caching is also viable but requires version/config keying to be correct in production where txs may span protocol upgrades.

### PoC Guidance

- **Target code**: `src/transactions/TransactionFrame.h` (add `mutable std::optional<uint32_t> mCachedSize`), `src/transactions/TransactionFrame.cpp` (modify `getSize()` to check/populate cache)
- **Change description**: (1) Add a `mutable std::optional<uint32_t> mCachedSize` member to `TransactionFrame`, following the pattern of `mContentsHash`/`mFullHash`. Modify `getSize()` to populate on first call and return cached value thereafter. (2) Optionally, cache the `FeePair` result from `computePreApplySorobanResourceFee` with a key of `(protocolVersion, sorobanConfig pointer)` to avoid redundant Rust FFI calls.
- **Correctness check**: All existing Soroban tests (`[soroban]` tag), parallel apply tests (`ParallelSorobanLedgerClose`), and the apply-load benchmark. The `getSize()` cache is trivially correct since `mEnvelope` is immutable. For fee caching, verify that the cache is invalidated when protocol version or Soroban config changes (which happens between ledger closes).
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` for `sac,TX=6400,T=8`. Compare total close time and profile `preParallelApply` duration. Target: 3-8% reduction in T=8 close time, concentrated in the serial pre-apply phase.
