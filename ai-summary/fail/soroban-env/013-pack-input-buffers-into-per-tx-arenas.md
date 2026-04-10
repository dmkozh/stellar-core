# H002: Pack Input Buffers Into Per-Transaction Arenas Instead of Hundreds of `CxxBuf`s

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Low
**Impact**: CPU / allocation churn
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The bridge should marshal a transaction's input footprint using one or a few
contiguous byte arenas plus offset metadata, not one heap-owned `CxxBuf` per
ledger entry / TTL entry / auth entry. Even when the bytes are transaction-
unique, the bridge should avoid an O(footprint) storm of tiny owned buffer
objects in the hot apply path.

## Mechanism

`addReads()` emits one `CxxBuf` for every ledger entry and another for every TTL
entry, while `invokeHostFunction()` builds additional `CxxBuf`s for auth,
host-function, resources, source-account, PRNG-seed, and ledger-info fields.
The benchmark's batched SAC path magnifies this: with
`APPLY_LOAD_BATCH_SAC_COUNT = 100`, `invokeBatchTransfer()` appends one source
balance key plus one destination balance key per recipient, so a single
transaction carries 101 read-write keys before accounting for read-only keys.

That translates into hundreds of separately owned buffers per invocation once
`mLedgerEntryCxxBufs` and `mTtlEntryCxxBufs` are populated. A flat arena +
offset-table representation would keep the XDR bytes contiguous, eliminate the
per-buffer `unique_ptr<vector<uint8_t>>` object allocations, and reduce CXX
container overhead and allocator traffic in the apply hot path.

## Trigger

Run the SAC apply-load benchmark with batching enabled (`APPLY_LOAD_BATCH_SAC_COUNT = 100`).
Each batched transfer produces a very wide read-write footprint, which forces
`InvokeHostFunctionOpFrame` to create hundreds of `CxxBuf`s before each Rust
call.

## Target Code

- `docs/apply-load-benchmark-sac.cfg:35` — benchmark uses `APPLY_LOAD_BATCH_SAC_COUNT = 100`
- `src/rust/src/bridge.rs:13-15` — one owned heap object per `CxxBuf`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:453-466` — one `CxxBuf` per ledger entry and TTL entry
- `src/transactions/InvokeHostFunctionOpFrame.cpp:529-550` — more per-tx `CxxBuf`s for auth / request fields
- `src/simulation/TxGenerator.cpp:1449-1512` — batched SAC footprint contains 1 source + N destination balance keys

## Evidence

The bridge currently constructs `rust::Vec<CxxBuf>` containers filled with
individually owned byte buffers. In the batched SAC case, the footprint width is
explicit in `invokeBatchTransfer()`: 101 read-write keys when `N=100`, plus
read-only keys. Since `addReads()` mirrors that footprint into two CXX vectors
(ledger + TTL), a single invocation already pays for hundreds of owned bridge
objects before the Rust host sees any bytes.

## Anti-Evidence

This requires replacing convenient `xdr_to_opaque()`-per-object helpers with a
custom packer/unpacker or a new bridge schema carrying offsets. It does not
remove the actual XDR serialization cost for unique write entries, so the gain
depends on allocator overhead and CXX container churn being large enough in the
wide-footprint SAC workload.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (mechanism is allocation pattern, not serialization caching)
**Failed At**: reviewer

### Trace Summary

Traced the full CxxBuf creation path from `addReads()` (InvokeHostFunctionOpFrame.cpp:360–503) through `toCxxBuf()` (TransactionUtils.h:372–376) which calls `xdr::xdr_to_opaque(t)` and wraps the result in `make_unique<vector<uint8_t>>`. Confirmed each CxxBuf requires 2 heap allocations (one for `unique_ptr`-managed vector object, one for vector's data buffer). Then verified footprint sizes across all benchmark scenarios and computed aggregate allocation overhead. The "hundreds of CxxBufs" claim is only true for the batched SAC config (`APPLY_LOAD_BATCH_SAC_COUNT=100`), which is NOT used in the standard benchmark matrix (which uses `sac_batch_size=1`).

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:448-466` — `addReads()` inner loop: `getLedgerEntryOpt(lk)` → `toCxxBuf(*entryOpt)` per footprint key, producing 1 ledger CxxBuf + 1 TTL CxxBuf per entry
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf<T>()`: `CxxBuf{make_unique<vector<uint8_t>>(xdr::xdr_to_opaque(t))}` — 2 heap allocations per CxxBuf
- `src/transactions/InvokeHostFunctionOpFrame.cpp:526-553` — `invokeHostFunction()`: creates CxxBufs for auth entries, hostFunction, resources, sourceID, basePrngSeed
- `src/rust/src/bridge.rs:13-15` — `CxxBuf` wraps `UniquePtr<CxxVector<u8>>`; Rust only reads via `as_slice()`
- `src/rust/src/soroban_proto_any.rs:443-458` — Rust passes `ledger_entries.iter()` and `ttl_entries.iter()` (each yields `AsRef<[u8]>`) to soroban host
- `scripts/run_apply_load_matrix.py:31-38` — `Scenario` dataclass: `sac_batch_size: int = 1` (standard matrix uses batch_size=1, NOT 100)
- `src/simulation/ApplyLoad.cpp:1150` — SAC instance readOnlyKeys: 1 key (instance only, SAC is built-in)
- `src/simulation/TxGenerator.cpp:766-790` — `invokeSACPayment()`: RO=1 key (instance), RW=2 keys (from + to) = 3 total footprint entries
- `src/simulation/TxGenerator.cpp:840-866` — `invokeTokenTransfer()`: RO=2 keys (code + instance), RW=2 keys = 4 total entries
- `src/simulation/TxGenerator.cpp:1487-1511` — `invokeBatchTransfer()`: RO=3 keys, RW=101 keys = 104 total entries (only with batch_size=100)

### Why It Failed

1. **Wrong trigger condition — the standard benchmark matrix uses batch_size=1, not 100.** The hypothesis's claim of "hundreds of CxxBufs per invocation" depends on `APPLY_LOAD_BATCH_SAC_COUNT=100`, which is set in the standalone `docs/apply-load-benchmark-sac.cfg` but NOT in the standard benchmark matrix (`scripts/run_apply_load_matrix.py`). The matrix `Scenario` dataclass defaults to `sac_batch_size=1`. With batch_size=1, each SAC TX has only 3 footprint entries → ~11 CxxBufs per TX (not "hundreds").

2. **Allocation overhead is <0.5% of baseline for all standard scenarios.** Per-TX CxxBuf counts and aggregate allocation costs:
   - **SAC T=1** (3200 TXs, batch_size=1): 3 footprint entries → ~11 CxxBufs/TX → 35,200 CxxBufs → 70,400 heap allocs at ~20ns = ~1.4ms alloc + ~1.4ms dealloc = ~2.8ms / 850ms baseline = **0.33%**
   - **Custom token T=1** (1600 TXs): 4 footprint entries → ~13 CxxBufs/TX → 20,800 CxxBufs → 41,600 allocs = ~1.7ms / baseline = **<0.3%**
   - **Soroswap T=1** (1000 TXs): ~10 footprint entries → ~25 CxxBufs/TX → 25,000 CxxBufs → 50,000 allocs = ~2ms / baseline = **<0.3%**
   All are well below the 5% Low severity threshold and within benchmark measurement noise.

3. **Even the batched case is proportionally small.** For the standalone batched config (3000 TXs, batch_size=100): 104 footprint entries → ~216 CxxBufs/TX → 648,000 CxxBufs → 1.3M allocs → ~52ms. But the batched baseline is much longer (each TX executes 100 transfers in the Soroban host), so the fraction remains <1%.

4. **Implementation complexity is disproportionate to the gain.** The arena approach requires: (a) a new packing format on the C++ side (flat buffer + offset table), (b) changing the `invoke_host_function` FFI signature from `&Vec<CxxBuf>` to the new arena type, (c) custom iterators on the Rust side to yield per-entry slices from the arena. This is a significant bridge restructuring for <0.5% improvement.

5. **Related hypotheses (008, 010, 012) already established the ceiling.** Prior investigations traced the same `addReads()` → `toCxxBuf()` → `invoke_host_function()` code path and computed the total C++-side bridge overhead (serialization + allocation) at <0.5% of baseline. The arena proposal targets only the allocation component of that already-small overhead.

### Lesson Learned

When evaluating allocation-optimization hypotheses, verify that the trigger condition matches the actual benchmark configuration. The standard benchmark matrix (`run_apply_load_matrix.py`) defines the scenarios and their parameters — standalone config files may use different parameters. Also, modern allocators (jemalloc/tcmalloc) service small allocations from thread-local caches in ~15-30ns; replacing hundreds of such allocations saves only tens of microseconds per transaction, which is negligible against the ~100-300μs total per-TX cost in the standard scenarios.
