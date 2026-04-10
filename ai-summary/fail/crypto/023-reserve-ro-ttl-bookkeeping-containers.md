# H023: Reserve RO-TTL Bookkeeping Containers Up Front

**Date**: 2026-04-10
**Subsystem**: crypto, transactions
**Severity**: Low
**Impact**: tiny hash-set/hash-map allocator churn in read-only TTL bookkeeping
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If the benchmarked workloads carried large read-only Soroban footprints, the
RO-TTL helper containers should reserve capacity before collecting read-only TTL
keys and buffered TTL bumps. That would avoid growing a small hash set or map
while the parallel-apply bookkeeping path walks the footprint.

## Mechanism

`buildRoTTLSet()` starts from an empty `UnorderedSet<LedgerKey>`, and
`mRoTTLBumps` likewise starts empty and grows as read-only TTL bumps are
buffered. If a transaction had dozens or hundreds of read-only Soroban keys,
reserving those containers from the read-only footprint size could avoid a few
rehash rounds and repeated `getTTLKey()`/`LedgerKey` hash work.

## Trigger

Run the benchmark models and profile `buildRoTTLSet()` and RO-TTL bump
bookkeeping inside `commitChangesFromSuccessfulTx()`. Compare against a build
that reserves the read-only TTL set and the RO-TTL bump map from the read-only
Soroban footprint cardinality.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:148-160` — `buildRoTTLSet()` inserts read-only Soroban TTL keys into an empty set
- `src/transactions/ParallelApplyUtils.h:109-112` — thread state owns the buffered `mRoTTLBumps` map
- `src/transactions/ParallelApplyUtils.cpp:167-174` — RO TTL bumps are accumulated into the map
- `src/transactions/ParallelApplyUtils.cpp:831-840` — every successful tx builds the RO TTL set before commit
- `src/simulation/TxGenerator.cpp:844-845` — `custom_token` transfer uses only `instance.readOnlyKeys`
- `src/simulation/ApplyLoad.cpp:2207-2208` — token instance read-only key set is only two entries
- `src/simulation/ApplyLoad.cpp:2962-2985` — soroswap add-liquidity uses seven read-only keys
- `src/simulation/ApplyLoad.cpp:3140-3149` — soroswap swap uses five read-only keys

## Evidence

The reserve opportunity is technically real: both the RO TTL set and bump map
start empty despite the read-only footprint size being known. The helper runs
on every successful parallel Soroban tx, so this is not dead code.

## Anti-Evidence

The benchmark read-only footprints are tiny. `custom_token` transfer reads two
keys, soroswap swap reads five, and even add-liquidity reads only seven. That
caps the number of rehash rounds at a handful of elements, leaving only a very
small constant-factor saving compared with the much larger write-footprint
containers.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The container-growth inefficiency exists, but the benchmarked RO footprints are
far too small for it to matter. Reserving a set/map that tops out around 2-7
elements cannot plausibly move ledger-close time by 5% or more.

### Lesson Learned

Capacity-planning hypotheses in this area only become meaningful when the
container cardinality gets into the tens or hundreds of `LedgerKey`s per tx or
per stage. For the apply-load benchmarks, that means chasing write-footprint
structures, not the read-only TTL helpers.
