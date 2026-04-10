# H006: Cache ContractData TTL key hashes inside ValueEntry

**Date**: 2026-04-10
**Subsystem**: storage (ledger)
**Severity**: Low
**Impact**: per-ledger CPU reduction in `InMemorySorobanState` contract-data maintenance
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once a `CONTRACT_DATA` entry is admitted to `InMemorySorobanState`, subsequent
hash-table operations on that stored entry should hash and compare a precomputed
32-byte TTL-key hash, not regenerate it by serializing the original
`LedgerKey` and running SHA256 again. Reads, TTL updates, data updates, and
deletes should reuse cached per-entry metadata.

## Mechanism

`InternalContractDataMapEntry::ValueEntry` does not store the TTL-key hash for
the entry it owns. Every call to `copyKey()` recomputes it through
`getTTLKey(LedgerEntryKey(*entry.ledgerEntry))`, which serializes the key and
runs `sha256(xdr_to_opaque(...))`. That cost lands on the hot path for
`mContractDataEntries.find(...)`, equality checks against stored entries, and
insertions of updated values during `updateContractData`, `updateTTL`, and
deletes. In Soroban-heavy ledgers this makes the in-memory state maintenance pay
cryptographic-hash cost repeatedly for metadata that is immutable once the entry
is stored.

## Trigger

Run any apply-load Soroban scenario with many `CONTRACT_DATA` mutations, such as
SAC or custom-token. Every ledger close that calls
`InMemorySorobanState::updateState(...)` and touches contract data will execute
many `find`/`erase`/`emplace` operations against `mContractDataEntries`, each of
which currently re-derives stored-entry TTL hashes.

## Target Code

- `src/ledger/InMemorySorobanState.h:136-173` — `ValueEntry::copyKey()` and `hash()` recompute the TTL-key hash from the stored `LedgerEntry`
- `src/ledger/InMemorySorobanState.h:242-248` — query-side lookups already reduce to a single cached `uint256`, showing the stored side is the missing half
- `src/ledger/InMemorySorobanState.cpp:76-80` — TTL updates hit `mContractDataEntries.find(...)`
- `src/ledger/InMemorySorobanState.cpp:92-110` — contract-data updates do `find` + erase/reinsert
- `src/ledger/InMemorySorobanState.cpp:192-201` — deletes do `find` before erase
- `src/ledger/LedgerTypeUtils.cpp:30-37` — `getTTLKey` performs `sha256(xdr_to_opaque(e))`

## Evidence

`QueryKey` already stores the derived TTL hash directly, but `ValueEntry`
recomputes it on demand from the full `LedgerEntry`. That means lookup uses a
cheap query hash against an expensive stored-entry hash/equality path. The
stored key is immutable for the lifetime of a `ValueEntry`, so this repeated
SHA256 work is pure metadata regeneration and can be replaced with one-time
computation at insert/update time.

## Anti-Evidence

Adding a cached `uint256` to every stored contract-data entry increases memory
footprint, which could reduce cache density. Query-side lookups would still pay
one `getTTLKey` computation for the incoming `LedgerKey`, so the optimization
only removes the stored-entry half of the cost. The real benchmark impact
depends on how much of ledger close time is currently spent inside
`InMemorySorobanState::updateState(...)`.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — duplicate of `ai-summary/reviewed/ledger/001-cache-valueentry-keyhash.md` and `ai-summary/fail/ledger/019-cached-ttl-key-hash-in-contract-data-map.md`
**Failed At**: reviewer

### Trace Summary

This hypothesis proposes caching the `uint256` TTL key hash inside `ValueEntry` to eliminate SHA-256 recomputation during `unordered_set` operations. This exact optimization has already been fully investigated at `ai-summary/reviewed/ledger/001-cache-valueentry-keyhash.md`: reviewed as VIABLE (Low/Informational severity), implemented as a PoC (POC_PASS with full test suite passing), and then **REJECTED at final review** based on independent benchmark data showing no stable improvement and regressions in multiple scenarios.

### Code Paths Examined

- `src/ledger/InMemorySorobanState.h:148-158` — `ValueEntry::copyKey()` and `hash()` confirmed to recompute SHA-256 (same as prior investigation)
- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData()` find+erase+emplace pattern (same as prior investigation)
- `src/transactions/ParallelApplyUtils.cpp:563-607` — `collectClusterFootprintEntriesFromGlobal()` pre-loads footprint keys, reducing `InMemorySorobanState::get()` calls to near zero

### Why It Failed

**Duplicate with completed investigation.** The identical optimization was:

1. Reviewed as VIABLE at `ai-summary/reviewed/ledger/001-cache-valueentry-keyhash.md` (severity downgraded from Medium to Low)
2. Also reviewed at `ai-summary/fail/ledger/019-cached-ttl-key-hash-in-contract-data-map.md` (severity downgraded to Informational, <1% impact)
3. PoC implemented and passed all tests (POC_PASS)
4. **REJECTED at final review** — independent apply-load benchmark matrix showed:
   - `sac,TX=3200,T=8` regressed p50 **-4.73%** / p95 **-4.65%**
   - `soroswap,TX=1000,T=8` regressed p50 **-9.13%**, p95 **-7.89%**, p99 **-5.38%**
   - Only isolated improvement: `custom_token,TX=1600,T=1` p99 **+6.98%**
   - Conclusion: extra 32-byte cached hash per entry increases memory pressure enough to erase the micro-optimization

The inefficiency is real but too cold to matter in the benchmark path. `collectClusterFootprintEntriesFromGlobal()` pre-loads all Soroban footprint keys into thread maps, so `InMemorySorobanState::get()` is rarely reached during parallel apply. The serial `updateState()` path processes only ~100-300 entries per ledger (not ~6,400 as hypothesized), yielding <0.5ms savings.

### Lesson Learned

This optimization has been fully cycle-tested (hypothesis → review → PoC → final benchmark) and conclusively rejected. The `ValueEntry::copyKey()` SHA-256 recomputation is a real micro-inefficiency but does not produce measurable benchmark improvement because: (a) the parallel apply path pre-loads entries into thread maps, bypassing `InMemorySorobanState::get()`, and (b) the serial `updateState()` path handles far fewer entries than the total transaction count suggests. Future hypotheses targeting this code path should demonstrate why the prior benchmark results would not apply.
