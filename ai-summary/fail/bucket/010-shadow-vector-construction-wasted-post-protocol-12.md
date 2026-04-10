# H010: Shadow vector construction in addBatchInternal is wasted work post-protocol-12

**Date**: 2025-07-21
**Subsystem**: bucket
**Severity**: Low
**Impact**: ledger-close serial CPU micro-overhead
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

In modern protocols (≥12, where `FIRST_PROTOCOL_SHADOWS_REMOVED` applies),
`addBatchInternal` should not build a shadow vector since shadows are unused.
The code should skip the 22 `shared_ptr` copies into the vector and the 22
`pop_back` calls.

## Mechanism

`BucketListBase::addBatchInternal` (BucketListBase.cpp:691-726) builds a
22-element shadow vector from all level curr/snap buckets on every ledger
close, then pops elements off as it walks levels. On modern protocols (≥12),
`BucketLevel::prepare` (line 307-311) replaces this shadow vector with an
empty one anyway:

```cpp
auto shadowsBasedOnProtocol =
    protocolVersionStartsFrom(snap->getBucketVersion(),
                              LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED)
        ? std::vector<std::shared_ptr<BucketT>>()
        : shadows;
```

This means the entire shadow construction and teardown is wasted work for all
modern protocol versions.

## Trigger

Run any apply-load benchmark on protocol version ≥ 12. Every ledger close
triggers `addBatchInternal`, which builds and discards the shadow vector.

## Target Code

- `src/bucket/BucketListBase.cpp:691-726` — shadow vector construction
- `src/bucket/BucketListBase.cpp:728-732` — shadow vector pop in loop
- `src/bucket/BucketListBase.cpp:307-311` — `prepare()` discards shadows on modern protocols

## Evidence

The shadow vector is built unconditionally but only consumed by `prepare()`,
which discards it for protocol ≥ 12. The construction cost is 22 shared_ptr
atomic reference count increments, and teardown is 22 atomic decrements plus
pop_backs.

## Anti-Evidence

22 shared_ptr copy/destroy operations cost approximately 0.5-2μs total. Against
ledger close times of 100-500ms, this is <0.001% of close time.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-21
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The absolute cost of 22 shared_ptr copy/destroy operations is approximately
0.5-2μs, which is <0.001% of total ledger close time. This is far below the
Informational threshold of <1%. While the code is technically wasted work,
the savings are unmeasurably small and not worth the code complexity of a
protocol-version-conditional path.

### Lesson Learned

Shadow vector construction in `addBatchInternal` is a legacy path that's
effectively dead code on modern protocols but costs negligibly. Future
optimizations should target the serialization/hashing/disk-write path which
dominates the level-0 close cost, not the bookkeeping around it.
