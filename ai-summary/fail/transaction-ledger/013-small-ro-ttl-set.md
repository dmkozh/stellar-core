# H013: Replace Per-Tx `buildRoTTLSet` Hash Tables with Small-Vector Membership

**Date**: 2026-04-10
**Subsystem**: transaction-ledger
**Severity**: Low
**Impact**: Per-transaction post-host bookkeeping
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

`commitChangesFromSuccessfulTx` should identify read-only TTL bumps without
allocating an `UnorderedSet` for every successful transaction. Since the
benchmark footprints are tiny and fixed-shape, a stack-backed vector or direct
footprint walk should be sufficient.

## Mechanism

`buildRoTTLSet` constructs a fresh hash set of TTL keys for every successful
transaction, and `commitChangesFromSuccessfulTx` uses it only for membership
checks while merging modified entries back into thread state. The thought was
that eliminating one small hash-table allocation plus a few `getTTLKey` /
`emplace` calls per tx might reduce post-host overhead in lightweight Soroban
workloads.

## Trigger

Run `scripts/run_apply_load_matrix.py` and compare current behavior against a
build that replaces `buildRoTTLSet` with a small fixed-capacity container or
linear search over the tx’s read-only footprint.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:148-161` — `buildRoTTLSet` allocates and fills an `UnorderedSet<LedgerKey>`
- `src/transactions/ParallelApplyUtils.cpp:832-840` — every successful tx builds that set before merging changes
- `src/simulation/ApplyLoad.cpp:1150-1153` — SAC has 1 read-only key
- `src/simulation/ApplyLoad.cpp:2207-2211` and `src/simulation/TxGenerator.cpp:840-845` — custom-token transfers have 2 read-only keys
- `src/simulation/ApplyLoad.cpp:3140-3149` — soroswap swaps have 5 read-only keys

## Evidence

- The helper does allocate a new hash container per successful tx.
- The benchmark executes thousands of successful Soroban txs per ledger, so the
  allocation pattern is real and repeated.
- The relevant footprint sizes are statically tiny in the benchmark scenarios:
  1 key for SAC, 2 for custom-token, 5 for soroswap.

## Anti-Evidence

- Because the benchmark footprints are so small, the current work is only a few
  inserts and hash computations per tx; the absolute saving is tiny.
- `getTTLKey` and the membership checks are dwarfed by larger bridge costs such
  as serializing shared read-only entries and processing host outputs.
- The optimization would complicate a correctness-sensitive TTL-bump path for a
  micro-level change that is unlikely to register in end-to-end ledger timing.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The benchmark footprints are simply too small for this container micro-tuning
to matter. Replacing one tiny `UnorderedSet` build per tx might shave a few
hashes and at most one allocation, but it does not attack any of the large
repeated costs on the apply-load path.

### Lesson Learned

When the footprint cardinality is bounded at 1-5 keys, prefer optimizations
that eliminate repeated serialization of large shared entries or test-only
instrumentation, not bookkeeping containers around those keys.
