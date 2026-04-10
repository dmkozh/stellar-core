# H018: InMemorySorobanState::getTTL Allocates shared_ptr Per Call

**Date**: 2025-07-14
**Subsystem**: ledger
**Severity**: Informational
**Impact**: Parallel apply throughput
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`InMemorySorobanState::getTTL()` should return TTL information without
heap-allocating a new `LedgerEntry` on each call. The TTL data (two uint32_t
fields) could be returned as a lightweight struct or the existing `TTLData`
type, avoiding the overhead of constructing a `shared_ptr<LedgerEntry>` with
a heap allocation.

## Mechanism

`getTTL()` (InMemorySorobanState.cpp:410-443) constructs a fresh
`shared_ptr<LedgerEntry>` via `std::make_shared<LedgerEntry>()` on every call
(line 420). It then populates the TTL fields from the stored `TTLData` and
returns the shared_ptr. This involves:
- One heap allocation (~50ns for the control block + LedgerEntry)
- LedgerEntry default construction + field assignment (~50ns)
- shared_ptr destruction on caller side when done (~30ns)

Total per call: ~130ns for allocation overhead.

Called from `getLiveEntryOpt` when the key is a TTL entry not yet in the
thread-local map. For SAC benchmark: ~4,000-8,000 first-touch TTL lookups
across all 8 worker threads. Total allocation overhead: ~0.5-1.0ms across
threads = ~0.06-0.13ms wall time.

## Trigger

Profile heap allocation patterns during parallel apply. `getTTL()` will show
up as a source of short-lived heap allocations in the worker threads.

## Target Code

- `src/ledger/InMemorySorobanState.cpp:410-443` — `getTTL` constructs
  `make_shared<LedgerEntry>()` on line 420 for every call
- `src/ledger/InMemorySorobanState.cpp:418-425` — `constructTTLEntry` lambda
  allocates and populates the LedgerEntry

## Evidence

The `getTTL` function explicitly constructs a new `LedgerEntry` on every call,
even though the caller typically only needs the `liveUntilLedgerSeq` and
`lastModifiedLedgerSeq` values. The `TTLData` struct (InMemorySorobanState.h:28-44)
already stores exactly these two fields and is much cheaper to return.

## Anti-Evidence

The interface returns `shared_ptr<LedgerEntry const>` to match the contract
of other InMemorySorobanState accessor functions (`get()` for CONTRACT_DATA
and CONTRACT_CODE). Changing the return type would require modifying all
callers. Additionally, the ~8,000 allocations at ~130ns each = ~1ms total
is spread across 8 threads (0.13ms wall time) — far below the Low severity
threshold.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-14
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The total wall-time impact is ~0.13ms (1ms across 8 threads), which is far
below the Low severity threshold (5% of T=8 close time = ~7.5-15ms). The
allocation pattern is also cache-friendly (LIFO, short-lived) and modern
allocators handle this efficiently. The fix would require interface changes
across multiple call sites for negligible performance benefit.

### Lesson Learned

Short-lived shared_ptr allocations in parallel worker threads are very cheap
(~130ns each including destruction). With 8 threads and typical allocator
thread-local caches, even 8,000 allocations have negligible wall-time impact.
Focus allocation optimization efforts on serial paths or cases with 100K+
allocations.
