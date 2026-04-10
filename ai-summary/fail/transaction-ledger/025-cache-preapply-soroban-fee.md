# H003: Cache `computePreApplySorobanResourceFee` Across Validation and Apply

**Date**: 2026-04-10
**Subsystem**: transaction-ledger
**Severity**: Low
**Impact**: Duplicate C++↔Rust resource-fee bridge work between validation and apply
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once a Soroban transaction has successfully computed its pre-apply `FeePair`
against a specific last-closed-ledger/configuration state, apply-time setup
should reuse that deterministic result instead of recomputing it from scratch in
the same ledger-close cycle.

## Mechanism

`TransactionFrame::checkValidWithOptionallyChargedFee` computes
`computePreApplySorobanResourceFee` during validation, and then
`TransactionFrame::commonPreApply` computes the same value again when the tx is
actually applied. In apply-load, every generated tx is validated before timing
starts, so the second call moves otherwise deterministic bridge work back into
the measured close path. `TransactionFrame` already uses mutable caches for
immutable tx-derived data such as `mContentsHash` and `mFullHash`, so a cache
keyed by ledger version plus the relevant LCL/config state would fit existing
patterns.

## Trigger

Instrument `rust_bridge::compute_transaction_resource_fee` and run
`scripts/run_apply_load_matrix.py`. The current code should show one call during
generation-time validation and another during apply/preParallelApply for the same
transaction payload.

## Target Code

- `src/transactions/TransactionFrame.cpp:1159-1218` — `computeSorobanResourceFee` / `computePreApplySorobanResourceFee`
- `src/transactions/TransactionFrame.cpp:1894-1925` — validation path computes the Soroban resource fee
- `src/transactions/TransactionFrame.cpp:2049-2097` — `commonPreApply` computes it again during apply
- `src/transactions/TransactionFrame.h:67-75` — `TransactionFrame` already maintains mutable caches for immutable per-tx derived data
- `src/simulation/ApplyLoad.cpp:2136-2148` — SAC benchmark validates each tx before the timed close
- `src/simulation/ApplyLoad.cpp:2336-2341` — custom-token benchmark validates each tx before the timed close
- `src/simulation/ApplyLoad.cpp:3201-3206` — soroswap benchmark validates each tx before the timed close

## Evidence

- The same helper is called from both validation and apply on the same immutable
  `SorobanResources`, tx size, and tx extension data.
- Apply-load explicitly places validation outside the timed section, making the
  apply-time recomputation pure duplicated work for the benchmark.
- The `TransactionFrame` class already caches immutable derived values, so this
  does not require inventing a new ownership model.

## Anti-Evidence

- The cache key must be precise enough to avoid reusing a fee computed against an
  older ledger version or changed Soroban fee configuration if a tx survives
  across ledgers.
- The bridge helper is arithmetic-heavy but not obviously dominant, so the
  end-to-end win may land closer to the low single digits unless combined with
  other prepass reductions.
- The optimization helps most when the same `TransactionFrame` object survives
  from validation to apply, which is true for the benchmark but should be
  verified on the live pipeline.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the complete call path from `computePreApplySorobanResourceFee` through the C++→Rust FFI bridge to the Rust `compute_transaction_resource_fee` function. The Rust function performs 7 multiply-divide operations (`compute_fee_per_increment`) plus 6 saturating additions — pure integer arithmetic with zero allocations, zero I/O, and zero serialization. The C++ side constructs two trivial POD structs (`CxxTransactionResources`, `CxxFeeConfiguration`) via direct field copies. The entire round-trip is ~100–200ns per call.

### Code Paths Examined

- `src/transactions/TransactionFrame.cpp:1204-1219` — `computePreApplySorobanResourceFee` delegates to `computeSorobanResourceFee` with declared resource values
- `src/transactions/TransactionFrame.cpp:1159-1195` — `computeSorobanResourceFee` constructs `CxxTransactionResources` (5 field copies) and calls `rustBridgeFeeConfiguration` (8 field copies), then crosses FFI
- `src/rust/src/soroban_invoke.rs:63-74` — Rust bridge dispatches to protocol-versioned module via `get_host_module_for_protocol`
- `src/rust/src/soroban_proto_any.rs:601-611` — Converts CXX types to native Rust types via `.into()` and calls host fee function
- `src/rust/soroban/p24/soroban-env-host/src/fees.rs:149-205` — Pure arithmetic: 7× `compute_fee_per_increment` (multiply + ceiling divide each) + 6 saturating adds, returns `(i64, i64)` tuple
- `src/rust/soroban/p24/soroban-env-host/src/fees.rs:418-421` — `compute_fee_per_increment` is a single `saturating_mul` + `div_ceil`
- `src/ledger/NetworkConfig.cpp:2849-2869` — `rustBridgeFeeConfiguration` copies 8 config fields into a POD struct
- `src/transactions/TransactionFrame.cpp:1923` — Validation call site in `checkValidWithOptionallyChargedFee`
- `src/transactions/TransactionFrame.cpp:2089` — Apply call site in `commonPreApply`

### Why It Failed

The duplicated computation is real but trivially cheap. The entire `compute_transaction_resource_fee` round-trip (C++ struct construction → FFI crossing → Rust type conversions → 7 multiply-divide ops → FFI return) costs ~100–200ns per call. At 3200 transactions (maximum benchmark load), eliminating the duplicate saves ~320–640µs per ledger — well under 0.1% of total close time (typically 200–800ms).

This falls squarely into the pattern established by fail/012 (caching `loadFromLedger` with ~2–190µs savings showed 10–36% regressions) and Meta-Pattern 3 ("cache-overhead exceeds cache-savings"). Adding a mutable cache member with ledger-version validation logic would introduce overhead (hash/compare for cache key, branch prediction for cache-hit check, memory for stored result) that approaches or exceeds the ~150ns being saved per call.

### Lesson Learned

Pure-arithmetic FFI bridge calls with no allocations, serialization, or I/O are extremely cheap (~100–200ns). Before caching any bridge call result, verify the bridge function's actual complexity — if it's just integer math on POD inputs, the cache management overhead will exceed the saved computation. The "C++↔Rust bridge" label can be misleading; not all bridge crossings involve expensive marshaling.
