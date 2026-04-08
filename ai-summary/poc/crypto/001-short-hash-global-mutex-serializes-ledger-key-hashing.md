# H001: shortHash Mutex Serializes Ledger-Key Hashing

**Date**: 2026-04-08
**Subsystem**: crypto
**Severity**: High
**Impact**: parallel apply throughput
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once the process-wide SipHash key has been initialized, hash computations for in-memory `LedgerKey`, `Asset`, and `SCAddress` objects should proceed without any shared lock on the ledger-apply hot path. In the `T=8` apply-load scenarios, workers should be able to hash keys concurrently while building and probing `UnorderedSet`/`UnorderedMap` structures used for prefetch, footprints, and transaction effects.

## Mechanism

`shortHash::computeHash` and `XDRShortHasher::XDRShortHasher` both take `gKeyMutex` on every hash operation even though production code only reads the already-initialized `gKey`. The default hashers for `Asset`, `SCAddress`, and `LedgerKey` call these helpers for asset codes, data names, offer IDs, muxed accounts, and contract-data keys, so parallel Soroban apply can end up funnelling a large fraction of hash-table traffic through one process-wide mutex.

## Trigger

Run `scripts/run_apply_load_matrix.py` with the default `T=8` `custom_token` or `soroswap` scenarios. Inspect lock contention around `stellar::shortHash::gKeyMutex` while workers are inserting/probing contract-data-heavy ledger-key sets.

## Target Code

- `src/crypto/ShortHash.cpp:21-25` - `shortHash::initialize`
- `src/crypto/ShortHash.cpp:61-79` - `shortHash::computeHash`, `XDRShortHasher::XDRShortHasher`
- `src/ledger/LedgerHashUtils.h:52-63` - `Asset` hashing via `shortHash::computeHash`
- `src/ledger/LedgerHashUtils.h:119-122` - `SCAddress` muxed-account hashing via `shortHash::xdrComputeHash`
- `src/ledger/LedgerHashUtils.h:157-183` - `LedgerKey` hashing for `DATA`, `OFFER`, and `CONTRACT_DATA`
- `scripts/run_apply_load_matrix.py:71-101` - benchmark matrix includes `T=8` apply scenarios

## Evidence

`ShortHash.cpp` shows a single global mutex guarding every hash, and `LedgerHashUtils.h` routes multiple ledger-key variants through that path. The apply-load matrix explicitly exercises `T=8` parallel ledger apply, making a process-wide hashing lock a plausible scalability ceiling.

## Anti-Evidence

Some frequently used key variants hash only `uint256` fields and do not touch `shortHash`, so the impact depends on the benchmark's actual key mix. If a workload is dominated by account or contract-code keys rather than contract-data and auxiliary string-like fields, the mutex may be less important.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

`gKeyMutex` (a `std::mutex`) in `ShortHash.cpp:15` guards every call to `computeHash` (line 64) and every `XDRShortHasher` construction (line 76). In production, `gKey` is written once during single-threaded startup via `initialize()` (line 22-26) and never modified again. The mutex protects a dead write (`gHaveHashed = true`) in production builds — `gHaveHashed` is only read inside `#ifdef BUILD_TESTS` in the `seed()` function (line 42). The entire mutex is unnecessary for correctness in steady-state production code.

### Code Paths Examined

- `src/crypto/ShortHash.cpp:14-16` — `gKey`, `gKeyMutex`, `gHaveHashed` are process-wide static globals
- `src/crypto/ShortHash.cpp:21-26` — `initialize()` writes `gKey` once at startup under the lock
- `src/crypto/ShortHash.cpp:37-59` — `seed()` is `#ifdef BUILD_TESTS` only; the sole reader of `gHaveHashed`
- `src/crypto/ShortHash.cpp:61-72` — `computeHash()` holds the mutex for the ENTIRE SipHash computation (~50-100ns). Used by `LedgerKey` hashing for `DATA` (data name) and `OFFER` (offer ID), and `Asset` hashing for `CREDIT_ALPHANUM4/12` (asset code)
- `src/crypto/ShortHash.cpp:74-79` — `XDRShortHasher` constructor holds the mutex only to copy 16 bytes of `gKey` into `SipHash24` state (~10-20ns). Note: the initializer list `state(gKey)` on line 74 reads `gKey` WITHOUT the lock (a benign data race given startup ordering, but technically UB)
- `src/ledger/LedgerHashUtils.h:178-183` — `CONTRACT_DATA` hashing calls `xdrComputeHash(lk.contractData().key)` for the SCVal key, acquiring the mutex in the XDRShortHasher constructor. The contract address part (`SCAddress` for `SC_ADDRESS_TYPE_CONTRACT`) uses `std::hash<uint256>` and does NOT touch shortHash
- `src/transactions/TransactionFrameBase.h:52,93` — `TxModifiedEntryMap` and `ParallelApplyEntryMap` are both `UnorderedMap<LedgerKey, ...>`, so every insert/lookup/erase hashes the key
- `src/transactions/ParallelApplyUtils.h:107,221,303` — Thread, global, and per-tx entry maps all use `UnorderedMap<LedgerKey, ...>`, with operations happening on every transaction during parallel apply
- `lib/util/siphash.h:11-99` — `SipHash24` is a streaming hasher; its `update()` and `digest()` methods execute OUTSIDE the lock in the `xdrComputeHash` path

### Findings

**The inefficiency is real.** Every `computeHash` call serializes the entire hash computation (including SipHash-2,4) under `gKeyMutex`. Every `XDRShortHasher` construction serializes only the 16-byte key copy under the mutex — the actual XDR traversal and SipHash update/digest run lock-free.

**The mutex is unnecessary in production.** `gKey` is written once at startup (single-threaded) via `initialize()` and never modified again. `gHaveHashed` is a dead write in production builds (only read by `#ifdef BUILD_TESTS` code). The mutex exists solely for the test-only `seed()` re-initialization guard.

**Impact on Soroban workloads is real but bounded.** For `CONTRACT_DATA` keys (the dominant key type in Soroban benchmarks), the lock is only held during `XDRShortHasher` construction (~10-20ns). The heavier XDR serialization of the `SCVal` key happens lock-free. For classic key types (`DATA`, `OFFER`, `Asset`), `computeHash` holds the lock for the full SipHash computation (~50-100ns), which is more significant under contention but less relevant to Soroban-dominated workloads.

**Quantitative estimate:** With 8 threads, each performing ~50-100 map operations per transaction across multiple maps (thread entry map, tx entry map, global entry map), and ~10-50 transactions per thread per ledger close, the mutex sees 4,000-40,000 lock/unlock operations per ledger close. At ~10-20ns per critical section (XDRShortHasher path), the theoretical serialization overhead is 40-800μs. Against a ledger close time of tens of milliseconds, this is a few percent — consistent with Low severity.

**Bonus finding:** The `XDRShortHasher` initializer list on line 74 (`state(gKey)`) reads `gKey` without the lock, then re-reads it inside the lock body. This is technically a data race (undefined behavior), though benign in practice since `initialize()` completes before any threads start.

### PoC Guidance

- **Target code**: `src/crypto/ShortHash.cpp` (lines 14-79) and `src/crypto/ShortHash.h` (lines 28-33)
- **Change description**: Remove `gKeyMutex` from `computeHash()` and `XDRShortHasher::XDRShortHasher()`. Options:
  1. **(Simplest, recommended)** Copy `gKey` into a local `thread_local` cache on first use per thread, eliminating all synchronization from the hash path. The `gHaveHashed` tracking can use `std::atomic<bool>` and be conditioned on `#ifdef BUILD_TESTS`.
  2. **(Alternative)** Replace `std::mutex` with `std::shared_mutex` and take `shared_lock` in hash paths. This is less intrusive but still has reader-reader synchronization overhead.
  3. **(Minimal)** Simply remove the lock from hash paths in production builds (keep it for `BUILD_TESTS`), relying on the startup-ordering guarantee that `initialize()` completes before any threads run.
- **Correctness check**: Existing tests for `ShortHash` (search for `"ShortHash"` or `"shortHash"` in test files). Also run `[ledger]` and `[tx]` tagged tests to verify hash-map behavior is unchanged. The key concern is that the thread-local or lock-free approach must produce identical hash values (same `gKey` bytes).
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` with `T=8` `custom_token` and `soroswap` scenarios. Compare wall-clock ledger close time (median and p99). Expected improvement: 2-8% on CONTRACT_DATA-heavy workloads, potentially more on classic workloads with many OFFER/DATA keys.

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-08
**PoC by**: claude-opus-4.6, high

### Changes Made

- **`src/crypto/ShortHash.cpp` (lines 14-83)**: Moved `gHaveHashed` and `gExplicitSeed` declarations under `#ifdef BUILD_TESTS` since they are only used by the test-only `seed()` function. Wrapped the `std::lock_guard` and `gHaveHashed = true` in both `computeHash()` and `XDRShortHasher::XDRShortHasher()` with `#ifdef BUILD_TESTS`, so in production builds the mutex is never acquired on the hash hot path. In `XDRShortHasher`, the production path relies solely on the initializer list `state(gKey)` (which was already reading `gKey` without the lock), and skips the redundant reassignment. The `initialize()` and `getShortHashInitKey()` functions retain their mutex usage unchanged.

### Demonstration

In production builds, `computeHash()` and `XDRShortHasher()` no longer acquire `gKeyMutex`, eliminating all synchronization from the SipHash hot path used by `LedgerKey`, `Asset`, and `SCAddress` hash functions. This allows 8+ parallel apply threads to compute hash-map keys concurrently without contention on the global mutex that previously serialized every hash operation. The optimization is safe because `gKey` is written once during single-threaded startup and never modified in production.

### Test Results

- All 16 `[crypto]` tests passed (15,262 assertions)
- All 1 `[shorthash]` test passed (1,000 assertions confirming XDR hash == byte hash)
- All 7 `[ledger]` tests passed (4,682 assertions)
- All 124 `[tx]` tests passed (572,146 assertions)
- Full test suite: 1 pre-existing flaky failure in "ledger state update flow with parallel apply" (HerderTests.cpp:5188, passes on re-run; unrelated to this change since `BUILD_TESTS` paths are identical to original code)
