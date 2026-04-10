# H009: Reduce SQLite Connection Pool Size for Single-Consumer Benchmark

**Date**: 2026-04-10
**Subsystem**: database
**Severity**: Informational
**Impact**: resource waste
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

The connection pool should match the actual number of concurrent consumers.
In the apply-load benchmark, only one pool connection is ever used (the
"ledgerClose" session checked out by `LedgerTxnRoot::Impl::addChild`).
Creating fewer pool connections would reduce setup time and memory overhead.

## Mechanism

`createPool()` allocates `hardware_concurrency / 2` connections (since
misc DB halves the pool). With 8 cores, that's 4 connections. Each is
opened to SQLite, configured with PRAGMA journal_mode=WAL,
cache_size=-20000 (80MB limit), mmap_size=104857600 (100MB), and has
the carray extension registered. Only 1 of these connections is ever
used during the benchmark.

## Trigger

Run any apply-load benchmark. The pool is lazily created on first
`getPool()` call. All but one connection remain idle throughout.

## Target Code

- `src/database/Database.cpp:706-738` — `createPool()` allocates N connections
- `src/database/Database.cpp:719-723` — Pool size = hardware_concurrency / 2
- `src/database/Database.cpp:727-734` — Each connection gets full PRAGMA setup

## Evidence

Pool creation is one-time overhead (outside measured benchmark time), but
3 unused connections each potentially claim cache_size pages and
mmap_size virtual address space.

## Anti-Evidence

1. Pool creation happens during `app.start()`, before benchmark timing
   begins — the setup cost doesn't affect measured close time.
2. SQLite `cache_size` is a limit, not a pre-allocation — unused
   connections have empty caches consuming no memory.
3. SQLite `mmap_size` maps on demand — pages are only mapped when
   accessed. Unused connections don't map any pages.
4. The `pthread_mutex` in the pool's `find_free()` does a linear scan
   of N sessions, but N ≤ 4 and it's called once per ledger — trivial.
5. Virtual address space for mmap is essentially free on 64-bit systems.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

Unused pool connections have zero performance impact during the
benchmark. Pool creation is outside measured time, SQLite resources
(cache, mmap) are allocated on demand (not pre-allocated), and the
pool mutex scan of 4 entries is O(1) in practice. There is no
measurable overhead from having unused connections.

### Lesson Learned

SQLite resource settings (cache_size, mmap_size) are limits, not
allocations. Unused connections don't consume the configured resources.
When evaluating pool overhead, distinguish between configuration limits
(no cost when unused) and actual resource consumption (cost proportional
to usage). Only pool connection SETUP is wasted, and that's a one-time
cost outside benchmark measurement.
