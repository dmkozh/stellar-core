# H002: Reuse Serialized Footprint Entries Instead of Re-XDRing Every Read

**Date**: 2026-04-09
**Subsystem**: soroban
**Severity**: High
**Impact**: CPU / allocation churn / parallel apply scalability
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once a ledger entry is already resident in `InMemorySorobanState` or a parallel
apply entry map, the bridge should be able to hand Rust either a cached encoded
form or a cheaply reusable view, instead of re-encoding the same `LedgerEntry`
and TTL data for every transaction that touches it. Repeated reads of unchanged
state across a ledger should not repeatedly pay full XDR serialization cost.

## Mechanism

`InvokeHostFunctionApplyHelper::addReads` serializes every footprint entry and
its TTL companion through `toCxxBuf`, even when the entry came from shared
in-memory Soroban state and is unchanged. In `T=8` apply-load runs this
serialization happens independently in multiple worker threads for the same hot
contract data, so the bridge burns CPU and allocator time before the Rust host
can even start; caching encoded entry bytes alongside the in-memory entry and
invalidating on write should remove a serial bottleneck that scales with
footprint size rather than with useful execution.

## Trigger

Run `custom_token` or `soroswap` in apply-load, especially `T=8`, and sample
CPU in `InvokeHostFunctionApplyHelper::addReads`. If the hypothesis is correct,
profiling should show substantial time in `xdr::xdr_to_opaque` for repeated
contract-data / TTL reads, with the same keys being re-encoded across many
transactions in a ledger.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::addReads:360-503` — serializes every live footprint entry and TTL entry into `CxxBuf`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionParallelApplyHelper::handleArchivedEntry:1024-1089` — reserializes restored entries and TTLs before handing them to Rust
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::getLiveEntryOpt:700-734` — fetches shared immutable entries from in-memory state / snapshot
- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:451-453` — bridge only needs iterators over encoded ledger and TTL entries

## Evidence

The C++ side reserves vectors for the footprint, then `emplace_back`s a freshly
encoded `CxxBuf` for each entry and TTL pair. Parallel apply already centralizes
shared immutable Soroban state in `InMemorySorobanState`, which means the same
entry objects are available for reuse but their encoded forms are thrown away and
recreated for every consuming transaction.

## Anti-Evidence

Caching encoded forms increases memory usage and requires invalidation whenever a
write mutates an entry or its TTL, so a naive cache could trade CPU for memory
pressure. The biggest wins likely require targeting only hot shared Soroban
entries rather than caching every classic entry indiscriminately.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (distinct from H001/003 which addressed per-tx immutable inputs, not shared footprint entries)

### Trace Summary

Traced the complete parallel apply path for Soroban footprint entry serialization.
Each transaction independently deep-copies shared RO entries from
`InMemorySorobanState` via `shared_ptr` dereference (ParallelApplyUtils.cpp:734)
and then serializes each copy to a new `std::vector<uint8_t>` via
`xdr::xdr_to_opaque` (InvokeHostFunctionOpFrame.cpp:453). The stage/cluster
validation rules (TxSetFrame.cpp:1949-1976) confirm that RO footprint keys are
explicitly allowed to overlap between clusters within a stage, so the same
contract code entry can be copied and serialized independently by every
transaction in the ledger.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:addReads:360-503` — iterates footprint keys, for each live Soroban entry calls `getLedgerEntryOpt(lk)` then `toCxxBuf(*entryOpt)`, producing a fresh XDR-serialized `CxxBuf`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:addReads:453` — `toCxxBuf(*entryOpt)` calls `xdr::xdr_to_opaque`, allocating a new `std::vector<uint8_t>` for each entry
- `src/transactions/TransactionUtils.h:toCxxBuf:372-376` — always allocates via `std::make_unique<std::vector<uint8_t>>(xdr::xdr_to_opaque(t))`
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::getLiveEntryOpt:700-735` — for entries not in `mThreadEntryMap`, falls through to `InMemorySorobanState::get(key)` which returns `shared_ptr<LedgerEntry const>`, then deep-copies via `std::make_optional(*res)` at line 734
- `src/transactions/ParallelApplyUtils.cpp:collectClusterFootprintEntriesFromGlobal:563-590` — only pre-copies entries that exist in the global RW entry map; RO-only entries remain in `InMemorySorobanState` and are fetched on demand
- `src/ledger/InMemorySorobanState.cpp:get:205-236` — returns `shared_ptr<LedgerEntry const>` without any cached serialized form
- `src/herder/TxSetFrame.cpp:1949-1959` — stage validation: RO keys from one cluster may overlap with RO keys from another cluster (only RO-vs-RW and RW-vs-RW cause conflicts)

### Findings

**The redundancy is real but the magnitude is modest:**

1. **Deep copy cost**: Every RO entry access from `InMemorySorobanState` produces a deep copy of the `LedgerEntry` (line 734: `std::make_optional(*res)`). For CONTRACT_CODE entries (10-25KB typical), this is ~3-5µs per copy.

2. **Serialization cost**: After copying, `toCxxBuf()` allocates a new vector and serializes the entire entry (~3-5µs for a 20KB code entry). The serialized bytes are structurally identical to what every other transaction produces for the same entry.

3. **Multiplier**: In `custom_token` (T=8), each transaction reads the same contract code + instance in its RO footprint. With 100+ transactions spread across ~50 clusters, the code entry alone is copied+serialized ~100 times. For `soroswap` with 3-4 contract code entries per tx, the multiplier is higher.

4. **Estimated per-tx savings**: For `custom_token` (~2 RO entries), caching saves ~13µs per tx out of ~450µs total (~3%). For `soroswap` (~4 RO code entries), caching saves ~40µs per tx out of ~750µs total (~5%). These are rough estimates.

5. **Not a "serial bottleneck"**: The hypothesis incorrectly characterizes this as a serial bottleneck. The serialization happens independently per-thread. It is redundant CPU burn, not a synchronization bottleneck.

6. **Architectural complexity**: The proposed fix requires either (a) caching serialized bytes in `InMemorySorobanState` alongside entries, with invalidation on write, or (b) pre-serializing shared RO entries before spawning parallel threads and plumbing them through `LedgerAccessHelper`. Both approaches require changes to the `LedgerAccessHelper` interface to expose pre-serialized data, and care must be taken with the `ScopedLedgerEntry` scope-checking system.

7. **The deep copy is a separate waste**: Even with cached serialization, each tx still deep-copies the `LedgerEntry` from the shared_ptr (line 734). Eliminating both the copy and serialization would require a larger refactor of the scope-checking system. Addressing serialization alone provides roughly half the benefit.

**Severity downgrade rationale**: The hypothesis claims High (>20%), but my analysis estimates 3-5% per-tx improvement, translating to roughly 2-4% of overall benchmark throughput. This is below the 5% Low threshold and classifies as Informational.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp` (addReads, lines 448-466), `src/ledger/InMemorySorobanState.h/.cpp` (entry storage), `src/transactions/ParallelApplyUtils.h/.cpp` (LedgerAccessHelper interface)
- **Change description**: Add a `std::shared_ptr<std::vector<uint8_t> const>` alongside each entry in `InMemorySorobanState` (lazily populated on first serialization, invalidated on entry update). Extend `LedgerAccessHelper` with an optional `getSerializedLedgerEntry(LedgerKey)` method. In `addReads()`, prefer the cached serialized form when available for Soroban entries.
- **Correctness check**: Existing Soroban test suite (`[soroban]` tag) covers the apply path. Parallel apply tests in `[tx]` tag cover the T>1 path. Verify that cached bytes are identical to fresh serialization, and that invalidation fires on every entry mutation.
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` with `soroswap` and `custom_token` at T=8. Measure median and p99 ledger close time. Expected improvement: 2-5% in the soroswap scenario, less for custom_token. Profile `addReads` time before/after to confirm the serialization overhead is reduced.
