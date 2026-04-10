# H003: Stop Materializing Heap-Allocated `TTLEntry` Objects on Every In-Memory Lookup

**Date**: 2026-04-10
**Subsystem**: transaction-ledger (ledger/InMemorySorobanState, transactions/ParallelApplyUtils, transactions/InvokeHostFunctionOpFrame)
**Severity**: Low
**Impact**: Repeated allocation in the in-memory Soroban TTL lookup path
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Repeated TTL reads should consume the cached `TTLData` directly or reuse a
stable lightweight representation, rather than allocating a fresh
`shared_ptr<LedgerEntry>` and then copying it into scoped state for every TTL
lookup.

## Mechanism

`InMemorySorobanState` stores TTL as sidecar metadata, but `getTTL` synthesizes
a new heap-allocated `LedgerEntry` on each lookup. `ThreadParallelApplyLedgerState::getLiveEntryOpt`
then copies that temporary object into a scoped optional value, so every TTL
probe on a cache hit pays both allocation and copy costs. `addReads` probes the
TTL for every Soroban footprint key before loading the underlying entry, which
makes shared instance/code keys in apply-load repeatedly materialize the same
TTL object thousands of times per ledger.

## Trigger

Run `scripts/run_apply_load_matrix.py` for `custom_token` or `soroswap` and
sample allocations in `InMemorySorobanState::getTTL` / `std::make_shared`.
The hypothesis is strongest when the same shared read-only contract keys appear
in most transactions of the ledger.

## Target Code

- `src/ledger/InMemorySorobanState.cpp:getTTL:410-443` — allocates a fresh `LedgerEntry` for every TTL lookup
- `src/ledger/InMemorySorobanState.cpp:get:204-235` — routes TTL keys through `getTTL`
- `src/transactions/ParallelApplyUtils.cpp:getLiveEntryOpt:699-734` — copies the returned shared entry into scoped optional state
- `src/transactions/InvokeHostFunctionOpFrame.cpp:addReads:377-410` — probes the TTL for every Soroban key before loading the entry itself
- `src/simulation/ApplyLoad.cpp:1147-1153,3140-3149` — apply-load reuses shared contract instance/code keys across many txs

## Evidence

- The `constructTTLEntry` lambda in `getTTL` always does `std::make_shared<LedgerEntry>()` and fills a new object.
- `getLiveEntryOpt` immediately copies `*res` into `scopeAdoptEntryOpt`, so the temporary shared object is not reused by the parallel-apply scope system.
- TTL lookups happen on the hot read path before host invocation, not only during rare archival or close-time code.

## Anti-Evidence

- The synthetic TTL entry is small, so the per-lookup savings may be modest unless the lookup count is very high.
- A non-allocating API may require threading a TTL-specific read path through code that currently expects `std::optional<LedgerEntry>`.
- If reviewed hypotheses that cache whole read-only inputs land first, they may already hide part of this cost for shared read-only keys.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (related but distinct from H010-cache-getttlkey-sha256 which targets SHA256 hashing, H018-return-typed-ttl-effects which targets bridge encoding, and H014-scope-adoption-deep-copies which targets scope copy overhead)
**Failed At**: reviewer

### Trace Summary

Traced the full TTL lookup path from `addReads` (InvokeHostFunctionOpFrame.cpp:370-503) through `getLedgerEntryOpt` → `TxParallelApplyLedgerState::getLiveEntryOpt` (line 886) → `ThreadParallelApplyLedgerState::getLiveEntryOpt` (line 769-804) → `InMemorySorobanState::get` (line 205, TTL branch at line 232) → `getTTL` (line 410-444). Confirmed the inefficiency: `getTTL` synthesizes a fresh `shared_ptr<LedgerEntry>` from compact `TTLData` (8 bytes) on every call, then `getLiveEntryOpt` copies via `std::make_optional(*res)` (line 803). However, the per-call cost is ~200-300ns and total call volume is modest (~5000-6000 per ledger in benchmark workloads), making the cumulative overhead <2ms — negligible against ledger close times of 2-20 seconds.

### Code Paths Examined

- `src/ledger/InMemorySorobanState.cpp:410-444` — `getTTL`: confirmed `make_shared<LedgerEntry>()` + field fill on every call. Also creates `make_unique<QueryKey>` for contract data map lookup (line 431 via `InternalContractDataMapEntry(ledgerKey)` constructor at InMemorySorobanState.h:242-258). Two heap allocations per call.
- `src/ledger/InMemorySorobanState.h:28-44` — `TTLData`: compact struct with just `liveUntilLedgerSeq` + `lastModifiedLedgerSeq` (8 bytes total). The source data is tiny; the materialized `LedgerEntry` wrapping adds ~60-100 bytes of overhead.
- `src/transactions/ParallelApplyUtils.cpp:769-804` — `ThreadParallelApplyLedgerState::getLiveEntryOpt`: for TTL keys not in `mThreadEntryMap`, falls through to `InMemorySorobanState::get` (line 796), then `std::make_optional(*res)` (line 803) copies the full LedgerEntry, then `scopeAdoptEntryOpt` wraps it.
- `src/transactions/ParallelApplyUtils.cpp:830-856` — `commitChangeFromSuccessfulTx`: RO TTL bumps go to `mRoTTLBumps` (line 845), NOT to `mThreadEntryMap`. This means RO TTL entries never get cached in the thread map — every TX re-fetches them from `InMemorySorobanState::getTTL`.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:378-411` — `addReads`: for every soroban key, calls `getTTLKey(lk)` (line 380) then `getLedgerEntryOpt(ttlKey)` (line 385). Extracts `ttlEntry = ttlEntryOpt->data.ttl()` (line 410) and later serializes via `toCxxBuf(*ttlEntry)` (line 462). The caller needs `TTLEntry` data (keyHash + liveUntilLedgerSeq), not the full `LedgerEntry` wrapper.
- `src/protocol-curr/xdr/Stellar-ledger-entries.x:530-534` — `TTLEntry`: just `Hash keyHash` (32 bytes) + `uint32 liveUntilLedgerSeq`.

### Why It Failed

**The inefficiency exists but the absolute overhead is far too small to produce measurable improvement.**

1. **Per-call cost is tiny**: `getTTL` materializes a TTL `LedgerEntry` (~60-100 bytes including shared_ptr control block) from 8 bytes of `TTLData`. The cost includes `make_shared` (~50ns), `make_unique<QueryKey>` for the data map lookup (~50ns), two hash table lookups (~100ns), field assignments (~10ns), and in `getLiveEntryOpt` an `optional` copy (~20ns) + scope adoption (~30ns). Total: ~260-360ns per TTL lookup.

2. **Call volume is modest**: For custom_token TX=3000: 2 RO keys × 3000 TXs = 6000 RO TTL lookups (always miss thread map) + ~60 RW TTL misses (first TX per cluster) ≈ 6060 `getTTL` calls. For soroswap TX=1000: 5 RO × 1000 + ~60 RW ≈ 5060 calls.

3. **Cumulative overhead is negligible**: 6060 × 310ns ≈ 1.88ms per ledger for custom_token. 5060 × 310ns ≈ 1.57ms for soroswap. Ledger close times for these workloads range from 2-20 seconds, making the TTL materialization overhead ~0.03-0.09% of total.

4. **Distributed across parallel threads**: With T=8, each thread handles ~375-750 TXs, so per-thread overhead is ~0.2-0.7ms — truly invisible.

5. **The proposed fix is high complexity for zero payoff**: Creating a non-allocating TTL read path requires a new API variant through the scope system, parallel apply helpers, and `addReads` (which currently expects `std::optional<LedgerEntry>` for uniform entry handling). This is invasive refactoring for <2ms savings.

6. **Precedent from H010**: The related H010 investigation (getTTLKey SHA256 caching) targeted ~9.8ms of overhead per ledger — 5× larger than this hypothesis — and its PoC caused benchmark regressions because cache management overhead exceeded savings. This hypothesis targets ~1.5-1.9ms, making it even less likely to produce a net positive.

7. **Partially redundant with H002**: The reviewed H002 hypothesis (cache readonly footprint CxxBufs) would cache the entire `addReads` pipeline for shared RO entries, including TTL lookups. If H002 lands, it would eliminate most of the `getTTL` calls this hypothesis targets.

### Lesson Learned

When evaluating heap allocation overhead on a hot path, multiply per-call cost (not just allocation cost, but the full round-trip including copies and wrapping) by actual call volume to get absolute overhead. For TTL entries (8 bytes of useful data wrapped in ~100 bytes of LedgerEntry), the materialization overhead per call is ~300ns — fast enough that even 6000 calls per ledger only total ~2ms. Optimizations targeting sub-2ms absolute overhead in a multi-second critical path are not viable unless the fix is trivially simple. Additionally, check for in-flight reviewed hypotheses (like H002) that may already subsume the targeted optimization as a side effect.
