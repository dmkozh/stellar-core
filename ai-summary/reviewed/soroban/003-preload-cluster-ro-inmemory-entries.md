# H003: Preload Shared RO In-Memory Entries Once Per Cluster

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Low
**Impact**: Parallel apply CPU / repeated deep copies from in-memory state
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Within a parallel-apply cluster, immutable Soroban read-only entries that are
shared across many transactions should be copied from `InMemorySorobanState` at
most once per thread state. Repeated transactions in the same cluster should hit
the thread-local entry map instead of deep-copying the same contract code or
instance entry on every `addReads()` call.

## Mechanism

`collectClusterFootprintEntriesFromGlobal` only preloads keys already present in
the global entry map, which mainly covers prior writes and modified classic
entries. Shared RO Soroban entries that live only in `InMemorySorobanState`
remain absent from `mThreadEntryMap`, so every transaction misses the thread map
and `getLiveEntryOpt` falls through to `InMemorySorobanState::get()` and
`std::make_optional(*res)`. In `custom_token` and `soroswap`, hundreds of
transactions in a cluster reuse the same code/instance keys, so these deep
copies repeat many times before serialization even begins.

## Trigger

Run apply-load `custom_token` or `soroswap` with `T=8` and profile
`ThreadParallelApplyLedgerState::getLiveEntryOpt` plus `addReads`. If the
hypothesis is correct, a meaningful share of pre-host time will be in
`std::make_optional(*res)` for RO contract code / instance entries that are
identical across many transactions in the same cluster.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::collectClusterFootprintEntriesFromGlobal:563-608` — preloads only keys already resident in the global map
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::getLiveEntryOpt:700-734` — falls through to `InMemorySorobanState::get()` and deep-copies missing RO entries
- `src/ledger/InMemorySorobanState.cpp:InMemorySorobanState::get:205-236` — returns shared immutable entries that are copied again by callers
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::addReads:360-466` — consumes those copied entries immediately for bridge marshaling
- `src/simulation/TxGenerator.cpp:invokeTokenTransfer:840-845` — every custom-token TX reuses the same instance read-only keys
- `src/simulation/ApplyLoad.cpp:2962-2985` — soroswap deposit path reuses router/factory/pair and SAC RO keys across many TXs

## Evidence

The thread-state preload path already exists; it just ignores RO entries that
are only present in `InMemorySorobanState`. The benchmark generators make the
sharing pattern explicit: custom-token transactions all read the same contract
code + instance, and soroswap transactions read the same router/factory/pair
code and instance keys repeatedly. That creates a cluster-local reuse
opportunity without needing cross-ledger invalidation.

## Anti-Evidence

This only removes the repeated deep copy, not the subsequent XDR serialization,
so its ceiling is lower than a full serialized-entry cache. Preloading more RO
entries into `mThreadEntryMap` also increases thread-state setup work and memory
footprint, so the win depends on clusters being large enough that the repeated
copies dominate the one-time preload cost.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full entry lookup chain from `addReads` through `TxParallelApplyLedgerState::getLiveEntryOpt` → `ThreadParallelApplyLedgerState::getLiveEntryOpt` → `InMemorySorobanState::get()`. Confirmed that RO Soroban entries not in the global entry map are never cached in `mThreadEntryMap`, causing repeated fallthrough to `InMemorySorobanState::get()` + `std::make_optional(*res)` for every TX in a cluster. However, the `ScopedLedgerEntryOpt` value-type semantics mean that **even hitting `mThreadEntryMap` still deep-copies the LedgerEntry** on each read (return by value from `getLiveEntryOpt`). Preloading saves exactly 1 copy per read (from 3 copies to 2), not all copies.

### Code Paths Examined

- `src/transactions/ParallelApplyUtils.cpp:collectClusterFootprintEntriesFromGlobal:562-608` — Confirmed: only fetches from `globalEntryMap`, skips entries only in InMemorySorobanState.
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::getLiveEntryOpt:699-735` — Confirmed: on cache miss, calls `InMemorySorobanState::get(key)` returning `shared_ptr<LedgerEntry const>`, then does `std::make_optional(*res)` (COPY #1) → `scopeAdoptEntryOpt(...)` (COPY #2). Result is not cached in `mThreadEntryMap`.
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::getLiveEntryOpt:885-904` — Calls through to thread state, then `scopeAdoptEntryOptFrom(...)` performs COPY #3 (scope transition from ThreadParApply to TxParApply).
- `src/ledger/LedgerEntryScope.h:ScopedLedgerEntryOpt:278-316` — `ScopedLedgerEntryOpt` stores `std::optional<LedgerEntry> mEntry` by value. All scope adoption and return-by-value operations copy the underlying LedgerEntry.
- `src/ledger/LedgerEntryScope.cpp:scopeAdoptEntryOptFromImpl:444-457` — Confirmed: `return ScopedLedgerEntryOpt<S>{mScopeID, entry.mEntry}` copies the optional<LedgerEntry> by const ref.
- `src/transactions/ParallelApplyUtils.h:TxParallelApplyLedgerState:296-303` — `mTxEntryMap` comment: "Merely loading data from the thread map or the live snapshot does not add an entry to this map." RO entries read during `addReads` are not stored, so `commitChangesFromSuccessfulTx` never promotes them to `mThreadEntryMap`.
- `src/ledger/InMemorySorobanState.cpp:get:204-236` — Returns `shared_ptr<LedgerEntry const>` from internal data structures (cheap, just refcount bump).

### Findings

The inefficiency is **real**: for N transactions in a cluster sharing K read-only Soroban keys, the InMemorySorobanState path performs 3 LedgerEntry deep copies per lookup (make_optional + scopeAdoptEntryOpt + scopeAdoptEntryOptFrom), while a thread map hit would perform 2 copies (return by value + scopeAdoptEntryOptFrom). Preloading saves N×K copies total.

However, the **impact is bounded** because:

1. **Two copies remain per read even after the fix.** The `ScopedLedgerEntryOpt` value-type design requires copying the LedgerEntry on every `getLiveEntryOpt` return (return by value from thread state) and every scope adoption (ThreadParApply → TxParApply). The optimization eliminates only the `make_optional(*res)` copy, saving 33% of copy work.

2. **XDR serialization still dominates.** After the copies, `addReads` immediately calls `toCxxBuf(*entryOpt)` which serializes the full LedgerEntry to XDR bytes — another allocation and copy that is not helped by this optimization.

3. **Estimated savings are small.** For custom_token T=8 with ~400 TXs/thread × ~3 shared RO keys: ~1200 avoided copies of ~10-40KB entries = ~12-48MB less allocation per thread. At ~1μs per copy, this is ~1.2ms per thread per ledger close, or ~240ms total across 200 ledgers. Against a total benchmark time of 30-60s, this is <1%.

**Severity downgraded from Low to Informational.** The finding is real and the fix is correct, but measurable benchmark improvement (≥5%) is unlikely.

### PoC Guidance

- **Target code**: `src/transactions/ParallelApplyUtils.cpp:collectClusterFootprintEntriesFromGlobal` — after the existing global map fetch loop, add a second pass that checks `InMemorySorobanState` for any footprint keys still missing from `mThreadEntryMap`.
- **Change description**: In `collectClusterFootprintEntriesFromGlobal`, for each key not found in `globalEntryMap`, call `mInMemorySorobanState.get(key)` and if non-null, emplace a `ThreadParallelApplyEntry::clean(scopeAdoptEntryOpt(std::make_optional(*res)))` into `mThreadEntryMap`. Do the same for the corresponding TTL key. This is safe because InMemorySorobanState is read-only during parallel apply.
- **Correctness check**: Existing parallel apply tests (`[soroban]` tag tests, `ParallelSorobanApply*` tests) cover this code path. The key correctness concern is RO TTL entries interacting with `flushRoTTLBumpsInTxWriteFootprint` — preloading them into `mThreadEntryMap` is fine because that function already reads from and writes to `mThreadEntryMap`.
- **Benchmark focus**: Run `custom_token T=8` and compare pre-host-invocation time. Improvement should be very small (<1% of total close time). Profile `getLiveEntryOpt` call frequency with and without the preload to verify the cache hit rate improvement.
