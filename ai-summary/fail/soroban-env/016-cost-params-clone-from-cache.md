# H016: ContractCostParams Clone Overhead from RwLock Cache

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Low
**Impact**: CPU / Rust-side bridge setup
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

After SUCCESS #001 (cache deserialized ContractCostParams on Rust side), the
cached `ContractCostParams` are cloned per TX invocation to construct the
`Budget`. The clone should be eliminated by either passing by reference or
using `Arc`-wrapped params that can be shared without deep copy.

## Mechanism

The `ProtocolSpecificModuleCache` (soroban_proto_any.rs:710-832) stores
deserialized `ContractCostParams` behind an `RwLock`. On each TX invocation,
the cache hit path clones both CPU and memory `ContractCostParams`:

```rust
let (cpu_params, mem_params) = {
    let cache = proto_cache.cost_params_cache.read().unwrap();
    (cache.cpu_cost_params.clone(), cache.mem_cost_params.clone())
};
```

Each `ContractCostParams` is `VecM<ContractCostParamEntry, 1024>`, essentially
a `Vec<ContractCostParamEntry>` with ~86 entries. Each entry contains an
`ExtensionPoint` (4 bytes) + `i64 constTerm` (8 bytes) + `i64 linearTerm`
(8 bytes) = ~24 bytes on Rust side.

Clone cost: 1 heap allocation (~40ns) + memcpy of ~2064 bytes (~50-100ns) =
~90-140ns per clone. Two clones: ~180-280ns per TX.

6400 TXs: ~1.15-1.79ms. Against 850ms baseline: ~0.14-0.21%.

## Trigger

Run any apply-load benchmark scenario with Soroban transactions.

## Target Code

- `src/rust/src/soroban_proto_any.rs:745-760` — Cost param cache hit path clones both params
- `src/rust/src/soroban_proto_any.rs:710-832` — `ProtocolSpecificModuleCache` structure

## Evidence

The cloned params are consumed by `Budget::try_from_configs()` which takes
ownership. Using `Arc`-wrapped params would require changing the Budget API
in soroban-env-host (out of scope). Passing by reference would require the
same API change.

## Anti-Evidence

The clone cost (~180-280ns per TX) is extremely small relative to the total
per-TX cost (~130-200μs). The savings are well below the noise floor of the
benchmark.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated (H005 investigated Arc sharing across shallow_clone, different angle)

### Why It Failed

The per-TX clone cost of ~180-280ns is 0.1-0.2% of the per-TX apply time.
With 6400 TXs, total savings of ~1.2-1.8ms represent ~0.14-0.21% of the
850ms baseline — well below the 5% Low severity threshold and below the
benchmark noise floor. Furthermore, eliminating the clone would require
changing the soroban-env-host `Budget::try_from_configs` API to accept
references or `Arc`-wrapped params, which is out of scope for bridge-layer
optimizations.

### Lesson Learned

After the SUCCESS #001 optimization eliminated per-TX XDR deserialization of
cost params, the remaining clone-from-cache overhead is negligible. Vec clones
of ~2KB are in the ~100-150ns range, which is below the threshold where
optimization matters for per-TX paths. Focus investigation effort on operations
that cost >1μs per TX to find meaningful improvements.
