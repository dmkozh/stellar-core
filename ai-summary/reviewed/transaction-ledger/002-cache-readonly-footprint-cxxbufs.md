# H002: Cache Shared Read-Only Footprint CxxBufs Across Invoke-Host Transactions

**Date**: 2026-04-10
**Subsystem**: transaction-ledger
**Severity**: High
**Impact**: C++↔Rust bridge marshalling overhead in Soroban apply
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Repeated reads of the same immutable read-only footprint entries within a ledger
close should reuse previously serialized `CxxBuf` payloads instead of
reloading and re-encoding the same `LedgerEntry` and `TTLEntry` bytes for every
transaction.

## Mechanism

`InvokeHostFunctionApplyHelper::addReads` serializes every footprint entry and
TTL with fresh `toCxxBuf` calls on every invoke-host tx. In apply-load, many
read-only keys are shared across the entire workload: SAC reuses one instance
entry, custom-token reuses the same contract-code plus instance entries, and
soroswap reuses router/pair code plus shared instance entries. Contract-code
entries are especially expensive because the serialized `LedgerEntry` includes
the full Wasm blob. A per-thread or per-stage cache keyed by `(LedgerKey,
liveUntilLedgerSeq)` should eliminate thousands of identical XDR serializations
and heap allocations per ledger.

## Trigger

Run `scripts/run_apply_load_matrix.py` and profile
`InvokeHostFunctionApplyHelper::addReads` / `toCxxBuf`. Compare current behavior
against a build that memoizes serialized read-only entry+TTL buffers for the
duration of a ledger or cluster. The strongest signal should be in
`custom_token,TX=1600,T=1|8` and `soroswap,TX=1000,T=1|8`, which repeatedly
ship contract-code entries across the bridge.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:360-466` — `addReads` reloads and serializes every footprint entry / TTL every tx
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf` always allocates and serializes a fresh byte vector
- `src/simulation/ApplyLoad.cpp:1150-1153` — SAC benchmark reuses the same instance key as read-only input
- `src/simulation/ApplyLoad.cpp:2207-2211` and `src/simulation/TxGenerator.cpp:840-845` — custom-token transfers reuse one contract-code key and one instance key in every tx
- `src/simulation/ApplyLoad.cpp:3140-3149` — soroswap swaps repeatedly ship router/pair code and shared instance keys as read-only inputs

## Evidence

- `addReads` does not have any cache, arena, or reuse path; every successful
  read goes through `toCxxBuf(*entryOpt)` and `toCxxBuf(*ttlEntry)`.
- Apply-load explicitly seeds shared read-only keys at scenario setup time:
  one SAC instance for XLM transfers, one contract-code + instance pair for the
  token workload, and shared router/pair code keys for soroswap.
- Contract-code keys are carried in the read-only footprint for
  `custom_token` and `soroswap`, meaning the bridge is repeatedly serializing
  large immutable Wasm-containing ledger entries, not just small account data.

## Anti-Evidence

- Read-write entries still need per-tx serialization, so the optimization only
  attacks the shared read-only subset.
- Read-only TTLs can change across ledgers (and in some cases via bumps), so
  the cache key must include the live-until value or be scoped tightly enough
  to avoid stale reuse.
- Soroswap's token pair selection varies, so not every tx reuses the exact same
  full read-only footprint.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (H007 in reviewed/ is related but distinct: H007 batches per-tx CxxBuf allocations, H002 caches across transactions)

### Trace Summary

Traced the complete read-only entry path from `addReads` (InvokeHostFunctionOpFrame.cpp:360-503) through `getLedgerEntryOpt` → `TxParallelApplyLedgerState::getLiveEntryOpt` (ParallelApplyUtils.cpp:886-901) → `ThreadParallelApplyLedgerState::getLiveEntryOpt` (ParallelApplyUtils.cpp:700-735) → `InMemorySorobanState::get` (InMemorySorobanState.cpp:205-236), and back through scope adoption, LedgerEntry copy, and `toCxxBuf` XDR serialization. Confirmed the inefficiency: for each read-only footprint entry per transaction, the code performs (1) two hash table lookups in entry maps, (2) a hash lookup in InMemorySorobanState, (3) a deep copy of the LedgerEntry including Wasm bytes via `std::make_optional(*res)`, (4) scope adoption wrapping, (5) a second copy when returning from `getLedgerEntryOpt`, and (6) full XDR serialization via `toCxxBuf`. For contract code entries with 20-50KB+ Wasm blobs, this is ~5-10µs per entry per transaction.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:360-503` — `addReads`: iterates footprint keys, calls `getLedgerEntryOpt` for each entry and TTL, serializes via `toCxxBuf`. No caching or memoization path exists.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:448-466` — For live soroban entries: `auto entryOpt = getLedgerEntryOpt(lk); auto leBuf = toCxxBuf(*entryOpt);` — fresh serialization every time.
- `src/transactions/ParallelApplyUtils.cpp:248-253` — `ParallelLedgerAccessHelper::getLedgerEntryOpt`: delegates to `mTxState.getLiveEntryOpt(key)` then `scopedOpt.readInScope(mTxState)`, returning `std::optional<LedgerEntry>` by value (copy).
- `src/transactions/ParallelApplyUtils.cpp:886-901` — `TxParallelApplyLedgerState::getLiveEntryOpt`: checks `mTxEntryMap` (miss for read-only), falls through to thread state.
- `src/transactions/ParallelApplyUtils.cpp:700-735` — `ThreadParallelApplyLedgerState::getLiveEntryOpt`: checks `mThreadEntryMap` (miss for unmodified read-only entries), falls to `InMemorySorobanState::get`, then `std::make_optional(*res)` — copies the full LedgerEntry from the shared_ptr.
- `src/ledger/InMemorySorobanState.cpp:205-236` — `get`: hash lookup in `mContractCodeEntries` or `mContractDataEntries`, returns `shared_ptr<LedgerEntry const>` (no copy), but caller copies.
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf<T>`: `make_unique<vector<uint8_t>>(xdr::xdr_to_opaque(t))` — allocates new vector, serializes XDR (for contract code, this is mostly length-prefixed memcpy of Wasm bytes).
- `src/protocol-curr/xdr/Stellar-ledger-entries.x:513-528` — `ContractCodeEntry` contains `opaque code<>` — the full Wasm bytecode, typically 10-100KB.
- `src/simulation/ApplyLoad.cpp:2207-2208` — custom_token setup: `mTokenInstance.readOnlyKeys` = [contractCodeKey, instanceKey] — both shared across all 1600 txs.
- `src/simulation/ApplyLoad.cpp:3140-3149` — soroswap swap: readOnly = [2 SAC instance keys, routerCodeKey, pairCodeKey] — code keys shared across all 1000 txs.
- `src/simulation/TxGenerator.cpp:840-845` — `invokeTokenTransfer`: `resources.footprint.readOnly = instance.readOnlyKeys` — directly reuses the shared keys.
- `src/rust/src/bridge.rs:13-15` — `CxxBuf { data: UniquePtr<CxxVector<u8>> }` — unique ownership per buffer, no sharing possible at the CxxBuf level.
- `src/rust/src/soroban_proto_any.rs:400-401` — Rust side borrows `ledger_entries: &Vec<CxxBuf>` and `ttl_entries: &Vec<CxxBuf>` by reference; does not take ownership.

### Findings

**The inefficiency is real and the fix is correct, but severity is Low, not High.**

**Per-transaction cost breakdown for shared read-only entries (parallel path):**
1. `TxParallelApplyLedgerState::getLiveEntryOpt` hash lookup in `mTxEntryMap` (miss): ~50ns
2. `ThreadParallelApplyLedgerState::getLiveEntryOpt` hash lookup in `mThreadEntryMap` (miss for unmodified RO entries): ~50ns
3. `InMemorySorobanState::get` hash lookup: ~100-200ns
4. `std::make_optional(*res)` deep copy of LedgerEntry including Wasm bytes (~50KB): ~2-3µs
5. Scope adoption + return copy: ~2-3µs
6. `toCxxBuf` XDR serialization + allocation (~50KB): ~2-3µs
7. TTL entry: same path but small (~36 bytes): ~0.3µs

**Total per read-only contract code entry per tx: ~5-10µs**

**Custom_token scenario (TX=1600):**
- 2 shared read-only keys (contract code ~20-50KB + instance ~1KB)
- Savings per tx from caching: ~6-8µs
- Total savings: 1600 × 7µs ≈ 11ms per ledger
- Estimated per-tx apply time (including Wasm execution): ~100-500µs
- Fraction saved: 1.4-7% of per-tx time

**Soroswap scenario (TX=1000):**
- 4+ shared read-only keys (2 SAC instances + router code + pair code)
- Router and pair Wasm blobs likely 50-100KB each
- Savings per tx: ~15-25µs
- Total savings: 1000 × 20µs ≈ 20ms per ledger
- Host execution is heavier for soroswap, so fraction may be smaller

**Why not High:**
- Soroban host execution dominates per-tx time. The bridge serialization overhead is real but secondary.
- The cache replaces LedgerEntry copies + XDR serialization with memcpy of cached bytes, so savings are ~50-60% of the original per-entry cost (not 100% — must still clone bytes into new CxxBuf).
- H007 (reviewed, Informational) quantified total CxxBuf overhead at 2-7% of per-tx time. H002 would save a subset of that (only read-only shared entries), but adds savings from skipping the `getLedgerEntryOpt` call chain.

**Secondary benefit under T=8:** Multiple threads accessing `InMemorySorobanState` for the same read-only keys create potential cache line contention on the shared data structure. A per-thread cache would reduce this contention, potentially providing larger gains under parallel execution than the raw serialization savings suggest.

**Correctness analysis:**
- Read-only footprint entries are immutable within a stage: only RW operations modify entries, and auto-restore only applies to RW footprint entries.
- TTLs for read-only entries don't change within a stage (TTL bumps are returned by the host only for entries in the RW footprint).
- A per-thread cache scoped to a cluster is safe because clusters are applied sequentially within a thread.
- Cache key should be `LedgerKey` (or its hash). TTL caching is safe because the TTL `liveUntilLedgerSeq` doesn't change for read-only keys within a stage.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp:360-503` (`addReads`), `src/transactions/ParallelApplyUtils.h/.cpp` (new cache structure)
- **Change description**: Add a per-thread `unordered_map<LedgerKey, pair<vector<uint8_t>, vector<uint8_t>>>` cache to `ThreadParallelApplyLedgerState` (or pass as a parameter through the parallel apply path). In `addReads`, for read-only entries (`isReadOnly=true`), check the cache before calling `getLedgerEntryOpt` + `toCxxBuf`. On cache hit, clone the cached bytes into a new CxxBuf via `make_unique<vector<uint8_t>>(cachedBytes)`. On cache miss, proceed with the normal path and populate the cache. Reset the cache between clusters (or scope it to the stage). For the pre-v23 sequential path, a similar optimization could be applied using a ledger-scoped cache on the `InvokeHostFunctionPreV23ApplyHelper`, but the parallel path is the primary target.
- **Correctness check**: Run `[soroban]` and `[tx]` tagged tests with `--ll fatal -r simple --abort --disable-dots`. Verify that cached entries produce identical CxxBuf bytes to non-cached entries (assert in debug builds). Pay special attention to auto-restore scenarios where entries transition from archived to live.
- **Benchmark focus**: `custom_token,TX=1600,T=1|8` and `soroswap,TX=1000,T=1|8`. Expected improvement: 5-10% of per-tx addReads/addFootprint time, translating to ~1-5% of total ledger close time. Use Tracy zones on `addReads` and `toCxxBuf` to measure the micro-level improvement. The SAC scenario has only 1 small read-only key (instance, no code), so improvement there will be minimal.
