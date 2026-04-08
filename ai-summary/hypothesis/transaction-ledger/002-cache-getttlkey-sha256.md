# H002: Cache getTTLKey SHA256 Computations to Eliminate Redundant Crypto Hashing

**Date**: 2026-04-08
**Subsystem**: transaction-ledger (ledger/LedgerTypeUtils, transactions/ParallelApplyUtils)
**Severity**: Low
**Impact**: 5-10% improvement on T=8 scenarios by eliminating redundant SHA256 in sequential bottlenecks
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`getTTLKey(ledgerKey)` should compute SHA256 of the XDR-serialized key at
most once per unique key per ledger close. Subsequent calls for the same
key should return a cached result.

## Mechanism

`getTTLKey` (LedgerTypeUtils.cpp:31-38) computes
`sha256(xdr::xdr_to_opaque(e))` — an XDR serialization + SHA256 hash —
every time it is called, with no caching. Multiple call sites invoke it
repeatedly for the same keys during a single ledger close:

1. `collectClusterFootprintEntriesFromGlobal` (line 601-603): calls
   `getTTLKey(key)` for every Soroban key in every tx's footprint
2. `getReadWriteKeysForStage` (line 111-113): calls `getTTLKey(lk)` for
   every readWrite Soroban key in every tx — called once per stage
3. `flushRoTTLBumpsInTxWriteFootprint` (line 639): calls `getTTLKey(lk)`
   for every readWrite Soroban key per tx
4. `InMemorySorobanState::get` for CONTRACT_DATA/CONTRACT_CODE: implicitly
   calls `getTTLKey` via `InternalContractDataMapEntry(ledgerKey)` constructor

For a ledger with 200 Soroban txs, 20 Soroban keys per tx footprint,
and 2 stages:
- Step 1: 200 × 20 = 4000 getTTLKey calls (sequential)
- Step 2: 200 × 10 (rw only) = 2000 calls per stage × 2 = 4000 (sequential)
- Step 3: 200 × 10 = 2000 calls (parallel across 8 threads)
- Step 4: varies, ~4000 calls (parallel)

Total: ~14000 getTTLKey calls. At ~700ns each (XDR serialize + SHA256):
~9.8ms per ledger. With only ~4000 unique keys, ~10000 are redundant,
wasting ~7ms.

Critically, steps 1 and 2 run sequentially on the apply thread, directly
extending the serial portion of the ledger apply. Per Amdahl's law, this
serial overhead limits T=8 scalability.

## Trigger

Run the apply-load benchmark with T=8 threads and soroswap or
custom_token transactions. Profile `getTTLKey` call frequency and total
time using Tracy or `perf record`.

## Target Code

- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey`: computes `sha256(xdr::xdr_to_opaque(e))` with no cache
- `src/transactions/ParallelApplyUtils.cpp:100-118` — `getReadWriteKeysForStage`: calls `getTTLKey` for each readWrite Soroban key
- `src/transactions/ParallelApplyUtils.cpp:562-607` — `collectClusterFootprintEntriesFromGlobal`: calls `getTTLKey` for each Soroban key
- `src/transactions/ParallelApplyUtils.cpp:626-659` — `flushRoTTLBumpsInTxWriteFootprint`: calls `getTTLKey` per readWrite key per tx
- `src/ledger/InMemorySorobanState.cpp:211-212` — `get()` for CONTRACT_DATA: constructs `InternalContractDataMapEntry` which calls `getTTLKey`

## Evidence

- `getTTLKey` implementation (LedgerTypeUtils.cpp:36): `k.ttl().keyHash = sha256(xdr::xdr_to_opaque(e));` — no caching whatsoever
- The same LedgerKeys appear in multiple call sites during a single ledger close (footprint keys are iterated in collectClusterFootprints, getReadWriteKeysForStage, and flushRoTTLBumps)
- SHA256 is a cryptographic hash function with ~500ns cost for small inputs
- `xdr::xdr_to_opaque(e)` allocates a vector and serializes, adding ~200ns
- CONTRACT_DATA keys can have large SCVal components (100-500 bytes), increasing serialization cost

## Anti-Evidence

- For CONTRACT_CODE keys, InMemorySorobanState uses the keyHash directly from TTL keys (no SHA256 needed for TTL key lookups)
- Some call sites work with TTL keys directly (already have the hash), bypassing getTTLKey
- The cost per call (~700ns) is small relative to VM execution time per tx (~2-5ms)
- Adding a cache introduces memory overhead and cache invalidation complexity
- A simple `unordered_map<LedgerKey, LedgerKey>` cache per stage would add ~40KB for 4000 entries but eliminate ~7ms of redundant hashing
