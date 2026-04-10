# H001: Cache TTL Key Hash in InternalContractDataMapEntry::ValueEntry

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Low
**Impact**: CPU reduction in Soroban entry lookups during parallel apply
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When looking up a ContractData entry in `InMemorySorobanState` via `mContractDataEntries.find()`, the `unordered_set` should compute the hash of the stored entry in O(1) using a cached value, and equality comparison should use the cached hash directly. The total cost of a lookup should be dominated by the hash table probe, not by cryptographic hashing.

## Mechanism

`InternalContractDataMapEntry::ValueEntry::hash()` calls `copyKey()` which calls `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))`. The `getTTLKey` function (LedgerTypeUtils.cpp:36) computes `sha256(xdr::xdr_to_opaque(e))` — a full SHA-256 hash plus XDR serialization — on **every** call. This means every `unordered_set` operation (find, insert, erase, rehash) on a `ValueEntry` triggers a fresh SHA-256 computation for the stored entry. Furthermore, `operator==` in `AbstractEntry` calls `copyKey()` on **both** operands, so collision resolution during lookups triggers two additional SHA-256 computations per bucket probe.

Note: This hypothesis is complementary to `transaction-ledger/002-cache-getttlkey-sha256` which addresses call-site caching in `ParallelApplyUtils`. This hypothesis targets the **internal data structure** overhead — the `unordered_set` that stores the entire in-memory Soroban state pays SHA-256 costs on every structural operation (insert, erase, rehash during `updateState`), and on equality checks during find. The fix is to cache the `uint256` TTL key hash inside `ValueEntry` at construction time.

## Trigger

Run the apply-load benchmark with any Soroban scenario. Operations on `mContractDataEntries` occur during:
- `InMemorySorobanState::updateState()` — called once per ledger to merge init/live/dead entries
- `InMemorySorobanState::get()` — called per-entry during parallel Soroban apply for entries not in thread/global maps
- `initializeStateFromSnapshot()` — called at startup

## Target Code

- `src/ledger/InMemorySorobanState.h:ValueEntry::copyKey():148-153` — recomputes SHA-256 on every call
- `src/ledger/InMemorySorobanState.h:ValueEntry::hash():155-158` — delegates to copyKey()
- `src/ledger/InMemorySorobanState.h:AbstractEntry::operator==():127-131` — calls copyKey() on both sides
- `src/ledger/LedgerTypeUtils.cpp:31-38` — getTTLKey computes sha256(xdr_to_opaque(e))
- `src/ledger/InMemorySorobanState.cpp:53-63` — updateContractDataTTL erases+reinserts (2 hash computations)
- `src/ledger/InMemorySorobanState.cpp:92-130` — updateContractData calls find (hash computation)

## Evidence

1. `ValueEntry::copyKey()` explicitly calls `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))` which computes `sha256(xdr::xdr_to_opaque(e))`.
2. The `hash()` method delegates to `copyKey()`, confirming no caching exists.
3. `operator==` calls `copyKey()` on both operands — two SHA-256 computations per equality check.
4. `updateContractDataTTL` (line 53-63) does erase+reinsert, paying hash cost twice.
5. The `QueryKey` class already demonstrates the pattern of storing the hash directly — `ValueEntry` should do the same.

## Anti-Evidence

1. `QueryKey` lookups (from `InMemorySorobanState::get`) only trigger `hash()` on the QueryKey side (which has the cached hash) — the `ValueEntry::hash()` is not called during find. Only `operator==` is called on bucket collision, which calls `copyKey()` on the ValueEntry.
2. With good hash distribution, collisions should be rare, so the `operator==` path is rarely hit.
3. The `updateState` path runs once per ledger and is sequential — it's not in the per-tx hot path.
4. If the `InMemorySorobanState::get()` call path is rarely reached (because entries are pre-loaded into thread maps), the impact may be smaller than expected.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced `ValueEntry::hash()` and `copyKey()` through all callers in `InMemorySorobanState.cpp`. Confirmed that `ValueEntry::hash()` recomputes SHA-256 on every call (via `getTTLKey` → `sha256(xdr_to_opaque(e))`). However, `find()` only calls `ValueEntry::hash()` indirectly through `operator==` on successful matches — not through the hash function itself (that uses the QueryKey's cached hash). The per-ledger `updateState()` path triggers ~220–440 SHA-256 computations total across all `find`/`emplace` operations, saving ~0.2–0.4ms per ledger. The invariant snapshot copy (which rehashes all N entries) is gated by `INVARIANT_EXTRA_CHECKS` and disabled in benchmarks.

### Code Paths Examined

- `src/ledger/InMemorySorobanState.h:148-158` — `ValueEntry::copyKey()` and `hash()` confirmed to call `getTTLKey()` → SHA-256 on every invocation
- `src/ledger/InMemorySorobanState.h:127-131` — `AbstractEntry::operator==()` calls `copyKey()` on both operands; triggers SHA-256 on ValueEntry side during every successful `find()`
- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey()` confirmed: `sha256(xdr::xdr_to_opaque(e))` every time
- `src/ledger/InMemorySorobanState.cpp:52-63` — `updateContractDataTTL()`: erase + emplace triggers 1 SHA-256 from emplace (ValueEntry::hash())
- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData()`: find triggers 1 SHA-256 from operator== on match, emplace triggers 1 more
- `src/ledger/InMemorySorobanState.cpp:114-142` — `createContractDataEntry()`: find on non-existent entry (no operator== SHA-256), emplace triggers 1 SHA-256
- `src/ledger/InMemorySorobanState.cpp:372-396` — Copy constructor: emplace for each entry triggers ValueEntry::hash() → SHA-256 per entry
- `src/ledger/LedgerManagerImpl.cpp:778-817` — `maybeRunSnapshotInvariantFromLedgerState()`: copy constructor is gated by `INVARIANT_EXTRA_CHECKS` config flag — disabled in benchmarks
- `src/transactions/ParallelApplyUtils.cpp:700-735` — `getLiveEntryOpt()`: falls through to `mInMemorySorobanState.get()` only when key is absent from `mThreadEntryMap` (pre-populated from footprints, so rarely reached)
- `src/transactions/ParallelApplyUtils.cpp:563-607` — `collectClusterFootprintEntriesFromGlobal()`: pre-loads all footprint keys into thread maps, reducing `InMemorySorobanState::get()` calls to near zero

### Findings

The inefficiency is **real** — `ValueEntry::hash()` and `copyKey()` recompute SHA-256 + XDR serialization on every call, and there is no caching. The proposed fix (cache `uint256` at construction time) is **correct** and doesn't break any invariants since entries are immutable once inserted.

However, the benchmark impact is **minimal** for these reasons:

1. **Per-ledger `updateState()` operations**: The number of `mContractDataEntries` operations per ledger is proportional to the transaction count (~100–300 entries changed), not the total state size. Each operation triggers 1–2 SHA-256 calls. At ~1μs per SHA-256+XDR serialization, total per-ledger cost is ~220–440μs. For a 200ms ledger close, this is ~0.1–0.2%.

2. **Parallel apply `get()` path**: `InMemorySorobanState::get()` is only reached when a key is missing from both thread and global entry maps. Since `collectClusterFootprintEntriesFromGlobal()` pre-loads all footprint keys, the `get()` path is almost never hit during normal Soroban execution.

3. **Rehash during steady-state**: The set size is stable during `updateState()` (creates ≈ deletes), so rehash events are extremely rare.

4. **Invariant snapshot copy**: Happens every ledger but is gated by `INVARIANT_EXTRA_CHECKS`, which is disabled in benchmarks. When enabled, this would save N SHA-256 computations (significant for large N), but it's not in the benchmark path.

5. **Initialization**: The copy saves ~2N SHA-256 from rehash during `initializeStateFromSnapshot()`, but this is a one-time startup cost.

Downgrading severity from **Low** to **Informational**: the improvement is real but too small to measure in apply-load benchmarks (<1% of ledger close time).

### PoC Guidance

- **Target code**: `src/ledger/InMemorySorobanState.h` — `ValueEntry` class (lines 136-174)
- **Change description**: Add `uint256 mCachedKeyHash` member to `ValueEntry`, computed once in the constructor via `getTTLKey(LedgerEntryKey(*entry.ledgerEntry)).ttl().keyHash`. Update `copyKey()` to return `mCachedKeyHash` and `hash()` to return `std::hash<uint256>{}(mCachedKeyHash)`. Update `clone()` to propagate the cached hash. Memory cost: 32 bytes per entry.
- **Correctness check**: Existing tests for `InMemorySorobanState` (search for `InMemorySorobanState` in test files, particularly `[soroban]` tagged tests) should pass unchanged since the behavior is identical — only internal implementation detail changes.
- **Benchmark focus**: Per-ledger `updateState()` time. Expected improvement: <1% (Informational). The invariant snapshot copy (if `INVARIANT_EXTRA_CHECKS` is enabled) would see a more noticeable improvement proportional to total contract data entry count.

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-10
**PoC by**: claude-opus-4.6, high

### Changes Made

- `src/ledger/InMemorySorobanState.h` (lines 136–187, `ValueEntry` struct):
  - Added `uint256 mCachedKeyHash` private member to cache the TTL key hash.
  - Added a private constructor accepting a pre-computed `uint256` hash, used by `clone()` to avoid recomputing SHA-256.
  - Modified the public constructor to compute `mCachedKeyHash` once via `getTTLKey(LedgerEntryKey(*entry.ledgerEntry)).ttl().keyHash`.
  - Updated `copyKey()` to return `mCachedKeyHash` (O(1) instead of SHA-256 + XDR serialization).
  - Updated `hash()` to use `mCachedKeyHash` directly.
  - Updated `clone()` to propagate `mCachedKeyHash` via the private constructor, avoiding SHA-256 recomputation during copy.

### Demonstration

The optimization eliminates redundant SHA-256 + XDR serialization from every `unordered_set` operation on `ValueEntry` (hash, equality, insert, erase, rehash, clone). The cached hash is computed once at construction time and reused for all subsequent operations. This is the same pattern already used by `QueryKey` and costs only 32 bytes of additional memory per entry.

### Test Results

Full test suite passed: `make check` with `NUM_PARTITIONS=$(nproc)` completed with "All 2 tests passed" (selftest-nopg and check-nondet partitioned test suites). All Rust tests also passed. No regressions detected.

---

## Final Review

**Verdict**: REJECTED
**Date**: 2026-04-10
**Final review by**: gpt-5.4, high
**Failed At**: final-review

### Adversarial Analysis

1. **Exercises claimed inefficiency**: YES — `ValueEntry::copyKey()` / `hash()` do recompute `getTTLKey()` and its SHA-256 work, and the patch removes that recomputation.
2. **Realistic preconditions**: NO — `collectClusterFootprintEntriesFromGlobal()` preloads most Soroban footprint and TTL entries into `mThreadEntryMap`, so `mInMemorySorobanState.get()` is not a normal hot-path lookup. The remaining `updateState()` work is once per ledger, and the expensive invariant snapshot copy is disabled with `INVARIANT_EXTRA_CHECKS=false`.
3. **Inefficiency vs by-design**: INEFFICIENCY — the recomputation is unnecessary for correctness, but it is too cold to matter in the measured apply-load path.
4. **Benchmark outcome**: FAIL — the independent apply-load matrix at `/home/devbox/apply-load/ledger-ttl-hash-review-20260410-102807/results.csv` shows no consistent end-to-end win. Measured deltas versus `ai-summary/baseline.csv`: `sac,TX=3200,T=8` regressed p50 **-4.73%** / p95 **-4.65%**; `soroswap,TX=1000,T=1` regressed p99 **-8.89%**; `soroswap,TX=1000,T=8` regressed p50 **-9.13%**, p95 **-7.89%**, p99 **-5.38%**. The only >5% improvement was `custom_token,TX=1600,T=1` p99 **+6.98%**, which is isolated and outweighed by broader losses.
5. **In scope**: YES — this is a ledger apply-path data structure.
6. **Benchmark methodology**: CORRECT — rebuilt the tree, ran `NUM_PARTITIONS=$(nproc) STELLAR_CORE_TEST_PARAMS='--ll fatal -r simple --abort --disable-dots' make check -j$(nproc)`, then ran `python3 scripts/run_apply_load_matrix.py --stellar-core-bin ./src/stellar-core --build-tag ledger-ttl-hash-review` against the saved baseline on the same host with 200 ledgers per scenario.
7. **Alternative explanations**: PLAUSIBLE — the small positive deltas fit normal run-to-run variance, and the extra 32-byte cached hash per contract-data entry plausibly increases memory pressure enough to erase the micro-optimization.
8. **Novelty**: LIKELY NOVEL — nothing in the reviewed materials suggested this was a duplicate.

### Rejection Reason

The patch removes a real micro-inefficiency, but that work is not material in the apply-load benchmark path. The independent matrix does not show a stable improvement and instead regresses multiple scenarios, especially soroswap with 8 threads. That is not enough evidence to keep the optimization in the optimized branch.

### Failed Checks

- 2 — realistic preconditions
- 4 — benchmark improvement / severity support
- 7 — alternative explanations
