# H002: Repeated RO footprint marshaling wastes Soroban apply CPU

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Medium
**Impact**: C++<->Rust bridge CPU / allocation churn
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When many transactions in the same cluster touch the same immutable read-only contract entries, apply should marshal those entries to Rust once per cluster or stage and reuse the serialized buffers. The hot loop should not repeatedly XDR-encode identical contract code, contract instance, and TTL entries for every transaction.

## Mechanism

`InvokeHostFunctionApplyHelper::addReads` loads each footprint key, converts the ledger entry and TTL entry to fresh `CxxBuf` objects with `toCxxBuf`, and appends them to per-tx vectors. The apply-load scenarios intentionally reuse the same RO objects across many txs: SAC benchmarks reuse the same SAC and batch-transfer instances, and Soroswap benchmarks reuse the router instance/code and pair code. Without a per-thread cache keyed by `LedgerKey` plus entry version/TTL, the bridge does repeated snapshot lookups, XDR serialization, and heap allocation for identical RO data on every host call.

## Trigger

Run `scripts/run_apply_load_matrix.py` and profile `soroswap,TX=1600,T=8` or `sac,TX=6400,T=8`. Expect visible CPU and allocation activity in `toCxxBuf`, XDR marshaling, and `std::vector<uint8_t>` construction beneath `InvokeHostFunctionApplyHelper::addReads`.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::addReads:360-503` - reloads and serializes every footprint key for every tx
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::addFootprint/invokeHostFunction:507-553` - feeds the freshly marshaled vectors into the Rust bridge
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSacPayments:2073-2111` - all txs in a cluster reuse the same batch-transfer and SAC contract IDs
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSoroswapSwaps:3140-3168` - every swap reuses router/code and pair-code RO keys

## Evidence

The RO-path marshaling code is plainly per-tx: `addReads` iterates the RO footprint, loads each live entry, calls `toCxxBuf(*entryOpt)` and `toCxxBuf(*ttlEntry)`, and pushes both into tx-local `rust::Vec<CxxBuf>` buffers. The Soroswap generator explicitly places router instance, two SAC instances, router code, and pair code in the RO footprint for every generated swap, making repeated serialization unavoidable in the current design.

## Anti-Evidence

RW entries cannot be safely shared the same way because later txs in the same cluster may observe prior writes, and archived-entry handling can also mutate restore bookkeeping. Any cache has to be limited to RO entries whose serialized form is stable for the lifetime of the thread or stage.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Each Soroban tx creates a fresh `InvokeHostFunctionParallelApplyHelper` (line 1274) which calls `addFootprint()` → `addReads(readOnly)`. For every RO footprint key, `addReads` (line 360) loads the entry via `getLedgerEntryOpt` (a cheap in-memory map lookup through `TxParallelApplyLedgerState` → `ThreadParallelApplyLedgerState` → `InMemorySorobanState`), then calls `toCxxBuf(*entryOpt)` (line 453) which invokes `xdr::xdr_to_opaque(t)` — a full XDR serialization into a freshly allocated `vector<uint8_t>`. This is repeated identically for every tx in the cluster that shares the same RO keys.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:addReads:360-503` — iterates each footprint key, calls `getLedgerEntryOpt` then `toCxxBuf` per entry per tx. No caching of serialized bytes.
- `src/transactions/TransactionUtils.h:toCxxBuf:372-376` — `CxxBuf{make_unique<vector<uint8_t>>(xdr::xdr_to_opaque(t))}` — always allocates and serializes from scratch.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:doParallelApply:1260-1279` — creates a new `InvokeHostFunctionParallelApplyHelper` per tx invocation, with fresh `mLedgerEntryCxxBufs` and `mTtlEntryCxxBufs` vectors.
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::getLiveEntryOpt:700-740` — entry loading is cheap (in-memory hash map lookups into `mThreadEntryMap` or `mInMemorySorobanState`). The waste is purely in post-load serialization.
- `src/rust/RustBridge.h:invoke_host_function:1154` — takes `const rust::Vec<CxxBuf>&` for ledger entries and TTL entries; the Rust side only reads, not consumes, these buffers.
- `src/herder/ParallelTxSetBuilder.cpp:57-60` — clustering only considers RW overlaps: "Transactions are considered to be dependent if they have the same key in their footprints and for at least one of them this key belongs to read-write footprint." RO-only overlap does NOT cause clustering, so txs sharing RO keys are distributed across different bins/clusters but each cluster still contains many txs sharing the same RO entries (bin-packed into ~8 clusters with ~N/8 txs each).
- `src/ledger/LedgerManagerImpl.cpp:2441-2449` — each cluster gets its own thread; within a thread, txs run sequentially, each independently serializing all RO entries.

### Findings

The inefficiency is confirmed and real. For the SAC TX=6400,T=8 benchmark:
- 6400 txs are bin-packed into ~8 clusters of ~800 txs each
- Each tx shares ~4 RO entries (SAC instance, SAC code, batch-transfer instance, batch-transfer code)
- Each tx independently calls `toCxxBuf` on all 4 RO entries = ~3,200 XDR serializations per thread
- A per-thread cache would reduce this to 4 serializations + 3,196 memcpy operations per thread

The `xdr_to_opaque` function performs a field-by-field traversal of the XDR object graph with discriminant branching, while a memcpy is a flat hardware-optimized copy. For contract entries in the 1KB-50KB range, the serialization has significantly higher constant factors than memcpy. However, the Soroban host VM execution still dominates total per-tx time, so the improvement on total apply time is bounded.

RO entries are safe to cache within a cluster/thread because: (1) they are not modified by any tx, (2) TTL bumps for RO keys are buffered separately in `mRoTTLBumps` and don't affect the entry data passed to the bridge, (3) `InMemorySorobanState` entries are `shared_ptr<LedgerEntry const>`, guaranteeing identity across lookups.

Correctness constraints are preserved: the Rust bridge receives `const` references to the CxxBuf vectors, so sharing pre-serialized bytes (via copy) is functionally identical to re-serializing. Thread safety is maintained because the cache is per-thread (one thread per cluster). The TTL entries for RO keys could theoretically be bumped by a prior tx in the cluster, but the RO TTL bumps go through a separate buffering path (`mRoTTLBumps`) and the `getLedgerEntryOpt` for TTL keys returns the original value during `addReads`. So TTL serializations for RO entries are also cacheable.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp` — add a per-thread serialization cache to `InvokeHostFunctionParallelApplyHelper::addReads` (or at the `ThreadParallelApplyLedgerState` level)
- **Change description**: Introduce an `unordered_map<LedgerKey, pair<vector<uint8_t>, vector<uint8_t>>>` on the `ThreadParallelApplyLedgerState` (or passed into each helper). On first access of an RO key, serialize and store the bytes. On subsequent accesses, create `CxxBuf` by copying from the cached bytes instead of calling `toCxxBuf`. Only cache entries from the `readOnly` footprint path (the `isReadOnly=true` call in `addFootprint`).
- **Correctness check**: Existing Soroban parallel apply tests (`src/transactions/test/ParallelApplyTest.cpp`) and the apply-load benchmark itself validate correctness. The optimization must produce byte-identical CxxBuf contents.
- **Benchmark focus**: Run `sac,TX=6400,T=8` and `soroswap,TX=1600,T=8` via `scripts/run_apply_load_matrix.py`. The metric to watch is total apply phase time (median and p99). Profile `addReads` specifically with Tracy/perf to measure the fraction of apply time spent in `toCxxBuf`. Expect 5-10% improvement on overall apply time for high-tx-count scenarios with shared RO footprints.

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-10
**PoC by**: claude-opus-4.6, high

### Changes Made

- **`src/transactions/ParallelApplyUtils.h`** (lines 113-127, 189-197): Added `mRoSerializationCache` (`mutable UnorderedMap<LedgerKey, std::vector<uint8_t>>`) to `ThreadParallelApplyLedgerState` and a `getRoSerializationCache()` accessor. The cache stores only serialized LedgerEntry bytes — TTL entries are deliberately excluded (see below).

- **`src/transactions/InvokeHostFunctionOpFrame.cpp`** (lines 328-343): Added virtual methods `serializeLedgerEntryForBridge()` and `serializeTtlEntryForBridge()` to `InvokeHostFunctionApplyHelper` base class with default `toCxxBuf` behavior. Modified `addReads()` (lines 468-482) to call these virtual methods instead of `toCxxBuf` directly.

- **`src/transactions/InvokeHostFunctionOpFrame.cpp`** (lines 1023-1025, 1190-1222, 1231): In `InvokeHostFunctionParallelApplyHelper`: added `mParallelThreadState` reference; overrode `serializeLedgerEntryForBridge()` to cache RO ledger entry bytes via `try_emplace`; overrode `serializeTtlEntryForBridge()` to NOT cache TTL entries (always delegates to `toCxxBuf`).

### Important Correctness Finding

The reviewer's guidance suggested caching both LedgerEntry and TTLEntry serializations. However, **TTL entries must NOT be cached** because `flushRoTTLBumpsInTxWriteFootprint()` can write buffered RO TTL bumps into `mThreadEntryMap` when a later RW tx in the same cluster overlaps. After flushing, `getLiveEntryOpt(ttlKey)` returns the bumped value, but the Rust host would receive stale cached bytes, causing the assertion `ttl(entry) >= ttl(oldEntryOpt.value())` at `ParallelApplyUtils.cpp:775` to fail. This was caught by the existing test "parallel txs / multi RO extensions with a single RW extension in a single stage and cluster."

The optimization still achieves the primary benefit: contract code and contract instance entries (1KB-50KB) are the expensive serializations, while TTL entries (~40 bytes) have negligible serialization cost.

### Demonstration

The optimization replaces redundant per-tx XDR serialization of shared read-only ledger entries with a per-thread cache that serializes each unique RO entry once and copies the cached bytes for subsequent transactions. For SAC TX=6400,T=8, this reduces ~3,200 XDR serializations per thread to ~4 serializations + ~3,196 memcpy operations, eliminating the field-by-field XDR traversal overhead for contract code/instance entries in the 1KB-50KB range.

### Test Results

All 21 `[parallelapply]` tests pass (2,797,082 assertions). All 68 `[tx][soroban]` tests pass (49,282 assertions). Full test suite (`make check` with NUM_PARTITIONS=$(nproc)) passes — all 2 test targets (selftest-nopg, check-nondet) succeeded.

---

## Final Review

**Verdict**: REJECTED
**Date**: 2026-04-10
**Final review by**: gpt-5.4, high
**Failed At**: final-review

### Adversarial Analysis

1. **Claimed inefficiency**: REAL — `InvokeHostFunctionApplyHelper::addReads` still pays full XDR serialization cost per read-only entry per transaction when no cache is present.
2. **Preconditions**: REALISTIC — apply-load scenarios do reuse contract code / instance entries heavily.
3. **By-design vs inefficiency**: INEFFICIENCY — the original repeated marshaling is wasteful, but the proposed cache is not behavior-preserving.
4. **Safety**: FAIL — the cache is keyed only by `LedgerKey` and lives for the entire `ThreadParallelApplyLedgerState`. Clusters explicitly include transactions that share a key when either side uses read-write footprint (`src/herder/ParallelTxSetBuilder.cpp:57-60`), and `LedgerManagerImpl::applyThread` reuses the same thread state across the whole cluster (`src/ledger/LedgerManagerImpl.cpp:2386-2407`). After a successful tx writes `K`, `commitChangesFromSuccessfulTx` updates `mThreadEntryMap` (`src/transactions/ParallelApplyUtils.cpp:832-843`) but nothing invalidates `mRoSerializationCache[K]`, so a later read-only tx on the same key can receive stale serialized bytes from `serializeLedgerEntryForBridge`.
5. **Independent validation**: FULL TEST SUITE PASSED — `NUM_PARTITIONS=$(nproc) STELLAR_CORE_TEST_PARAMS='--ll fatal -r simple --abort --disable-dots' make check -j$(nproc)` succeeded, which means this is a coverage gap rather than a CI-visible regression.
6. **Benchmarking**: NOT RUN — rejected before benchmarking because correctness failed first.

### Rejection Reason

The optimization is semantically unsafe. It assumes a key that is read-only in one transaction is immutable for the lifetime of the cluster, but the parallel apply scheduler deliberately clusters `RO(K)` and `RW(K)` transactions together. The new cache survives across those sequential transactions, so `RO(K) -> RW(K) -> RO(K)` can reuse stale serialized ledger-entry bytes after the write commits.

### Failed Checks

- Step 6 (Verify Safety)
