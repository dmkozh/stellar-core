# H003: Cache Serialized Shared Footprint Entries Across Transactions In A Ledger Close

**Date**: 2026-04-10
**Subsystem**: crypto, rust
**Severity**: Medium
**Impact**: repeated XDR size-pass, allocation, and serialization of identical live entries before FFI
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If many Soroban transactions in the same ledger close read the same immutable
ledger entries — contract code, contract instances, router/factory state, pair
code, shared balance objects — stellar-core should serialize those shared
entries once and reuse the encoded bytes until the entry becomes dirty. The hot
apply path should not re-run `xdr_to_opaque` for the exact same live entry every
time another transaction references it.

## Mechanism

Parallel apply already shares live entry objects aggressively: a thread state
preloads entries from `mGlobalEntryMap`, and misses then read from the
in-memory Soroban state or live snapshot. But `InvokeHostFunctionApplyHelper`
throws away that sharing right before the bridge by calling `toCxxBuf(*entryOpt)`
for every footprint read on every transaction. In the apply-load workloads, many
transactions reuse the same read-only entries repeatedly (for example the same
router instance, router code, pair code, token SAC instances, or token contract
code). A per-thread or global cache of serialized bytes for clean shared entries
— invalidated when the corresponding entry becomes dirty in the thread/global
maps — would eliminate repeated XDR size-pass + allocation + serialization work
for those reused live entries.

## Trigger

Run `custom_token` or `soroswap` apply-load and profile `xdr::xdr_to_opaque`
inside `InvokeHostFunctionApplyHelper::addReads`. Look specifically for the same
router/code/instance entries being serialized across many transactions in the
same ledger close. Compare against a build that caches encoded bytes for clean
global/thread entries and reuses them until the entry is dirtied.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:563-608` — thread state preloads shared footprint entries from the global map for the whole cluster
- `src/transactions/ParallelApplyUtils.cpp:699-735` — later reads fall back to shared thread/global/snapshot entries
- `src/transactions/InvokeHostFunctionOpFrame.cpp:369-467` — `addReads` reserializes every loaded entry into fresh bridge buffers for each tx
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf` always performs a fresh `xdr::xdr_to_opaque`
- `src/simulation/ApplyLoad.cpp:2252-2277` — `custom_token` transactions all reuse the same token code/instance entries while varying only account balance keys
- `src/simulation/ApplyLoad.cpp:3140-3168` — every `soroswap` swap reuses the same router instance, router code, pair code, and SAC read-only entries across many txs

## Evidence

The data-sharing layer is already present: global and thread state keep common
entries alive across transactions, and the benchmark workloads explicitly build
many transactions with repeated read-only keys. What is missing is only the
serialized form: the bridge preparation step ignores that shared object graph
and recomputes identical XDR buffers every time a transaction touches a shared
entry.

## Anti-Evidence

Write-set entries that change every transaction cannot safely reuse cached bytes
after they become dirty, so the benefit is concentrated in shared clean entries.
The cache therefore needs precise invalidation or "clean-only" scoping to avoid
serving stale serialized bytes.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — duplicate of soroban-env fail #008+#012 (condensed in `ai-summary/fail/soroban-env/summary.md`: "Cache or share pre-serialized read-only footprint entries per thread across TXs")
**Failed At**: reviewer

### Trace Summary

Traced the complete entry loading and serialization path. Confirmed that `addReads` (InvokeHostFunctionOpFrame.cpp:369-467) calls `toCxxBuf(*entryOpt)` per footprint key per transaction, and that shared RO entries (router code, pair code, SAC instances) are indeed re-serialized across all transactions in a cluster. However, for the largest shared entries (contract code with `opaque code<>` fields of 30-60 KB), XDR serialization cost is dominated by the `memcpy` of the WASM code bytes. The CxxBuf type (`UniquePtr<CxxVector<u8>>`) enforces unique ownership, so even with a serialization cache, each `CxxBuf` construction requires a full `memcpy` of the cached bytes into a fresh allocation — the dominant cost is not eliminated.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:448-466` — `addReads` calls `toCxxBuf(*entryOpt)` for each live entry, producing per-TX `CxxBuf` with `make_unique<vector<uint8_t>>(xdr::xdr_to_opaque(t))`. Confirmed N re-serializations of shared entries.
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf<T>` wraps `xdr_to_opaque(t)` in a `UniquePtr<vector<uint8_t>>`. The `UniquePtr` enforces unique ownership per CxxBuf.
- `src/rust/src/bridge.rs:13-15` — `CxxBuf { data: UniquePtr<CxxVector<u8>> }` — CXX's `UniquePtr` has no shared ownership variant for mapped types like `CxxVector<T>`.
- `src/protocol-curr/xdr/Stellar-ledger-entries.x:513-528` — `ContractCodeEntry` contains `opaque code<>` (unbounded bytes). For SAC token WASM (~46 KB), soroswap router (~50 KB), this dominates serialization cost.
- `src/transactions/ParallelApplyUtils.cpp:562-608` — `collectClusterFootprintEntriesFromGlobal` preloads shared entries into `mThreadEntryMap`, confirming data-level sharing exists but serialization-level sharing does not.
- `src/simulation/ApplyLoad.cpp:3140-3149` — Soroswap transactions share 5 RO entries (routerInstance, 2× SAC instances, routerCode, pairCode) across all TXs.

### Why It Failed

1. **Duplicate of soroban-env fail #008+#012.** That condensed investigation examined the identical proposal: "Cache or share pre-serialized read-only footprint entries per thread across TXs." It concluded: "For large WASM entries serialization cost is already dominated by memcpy of the code bytes; cached bytes still require per-CxxBuf memcpy into new owned buffer; aggregate savings <0.3% of baseline across all standard scenarios."

2. **CxxBuf unique ownership forces per-TX memcpy.** As documented in soroban-env fail summary meta-pattern #4: "CxxBuf forces unique ownership (`UniquePtr<CxxVector<u8>>`), so any caching still requires a memcpy into a new owned buffer." For a 46 KB WASM entry, `memcpy` takes ~3-5 µs. With caching, the savings is only the XDR field-traversal overhead (~1-3 µs per entry), not the data copy. Soroban-env fail #024 confirmed that zero-copy shared ownership through CXX is not viable due to CXX type system constraints.

3. **Aggregate savings ceiling ~0.1-0.6%.** For soroswap TX=1000: 5 shared RO entries × ~2-6 µs savings (XDR traversal only) × 1000 TXs = 10-30 ms. Against typical 2-5 second ledger closes, this is 0.2-0.6%. For custom_token TX=1600: 2 shared entries × ~2 µs × 1600 = 6.4 ms ≈ 0.2%. Well below the 5% threshold for Low severity.

### Lesson Learned

This is the third investigation (after soroban-env #008+#012 and #024) targeting serialization caching for shared footprint entries. All converge on the same fundamental barrier: CXX's `UniquePtr<CxxVector<u8>>` forces per-consumer memcpy, and for large entries (WASM code), that memcpy is the dominant serialization cost. Only a bridge API redesign (e.g., passing raw pointers with lifetime guarantees) could eliminate this copy, and that introduces unsafe code in a correctness-critical path.
