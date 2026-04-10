# H017: Bulk Encode Modified Output Entries into Single Buffer

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Low
**Impact**: CPU / FFI output marshaling
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

The Rust bridge should return modified ledger entries as a single contiguous
byte buffer with an accompanying offset/length table, rather than as individual
`RustBuf` vectors. This would reduce per-entry heap allocations on the return
path and improve cache locality.

## Mechanism

In `extract_ledger_effects()` (soroban_proto_any.rs:261-301), the Rust bridge
constructs a `Vec<RustBuf>` for `modified_ledger_entries`. Each `RustBuf` wraps
a `Vec<u8>` that holds one serialized `LedgerEntry`. For a typical SAC
transfer with ~3-6 modified entries (2-3 contract data entries + their TTL
entries), this means 3-6 individual heap allocations for the `Vec<u8>` backing
stores, plus the `Vec<RustBuf>` itself.

On the C++ side, each `RustBuf` is received as a `CxxBuf` (UniquePtr to
std::vector<uint8_t>), requiring additional heap management.

If all entries were packed into a single `Vec<u8>` with a separate offset
table `Vec<(u32, u32)>` (offset, length pairs), we'd:
1. Replace N heap allocations with 1 (for the combined buffer)
2. Improve cache locality during iteration
3. Reduce CXX bridge overhead (fewer unique_ptr transfers)

Per-entry heap alloc/dealloc: ~40ns × 2 (Rust alloc + CXX wrapper) = ~80ns.
For ~5 entries per TX: ~400ns savings.
6400 TXs: ~2.56ms. Against 850ms baseline: ~0.3%.

## Trigger

Run any apply-load benchmark scenario with Soroban transactions.

## Target Code

- `src/rust/src/soroban_proto_any.rs:261-301` — `extract_ledger_effects` builds per-entry RustBuf vectors
- `src/rust/src/bridge.rs:34-55` — `InvokeHostFunctionOutput` struct defines per-entry vector fields
- `src/transactions/InvokeHostFunctionOpFrame.cpp:610-703` — `recordStorageChanges` iterates modified entries

## Evidence

The pattern of many small buffers crossing FFI is a known anti-pattern for
CXX bridges. Batching into larger buffers reduces per-item overhead.

## Anti-Evidence

1. The per-entry overhead (~80ns) is very small. Total savings (~2.56ms for
   6400 TXs) represent only ~0.3% of baseline.
2. Requires non-trivial refactoring of both Rust and C++ bridge code to handle
   offset-based buffer access instead of individual buffers.
3. The C++ side needs individual entries for `xdr_from_opaque` decoding anyway,
   so the bulk buffer would need to be sliced back into individual spans,
   partially negating the benefit.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PARTIAL — related to H013 (pack input buffers into per-TX arenas) which covered the input side; this covers the output side

### Why It Failed

The per-entry FFI overhead (~80ns) is too small to produce meaningful savings.
With ~5 entries per TX and 6400 TXs, the total savings of ~2.56ms are ~0.3%
of the 850ms baseline — well below the 5% Low threshold and within the
benchmark noise floor. Additionally, the C++ consumer (`recordStorageChanges`)
needs individual entry byte spans for `xdr_from_opaque` decoding, so a bulk
buffer would need to be re-sliced, adding complexity without eliminating the
fundamental per-entry processing cost.

### Lesson Learned

Batching small FFI buffers into larger ones only provides meaningful savings
when the per-item overhead is >1μs or the item count is very large (>1000 per
TX). For ~5 entries per TX with ~80ns per-item overhead, the optimization is
below the noise floor. This pattern (packing buffers) was also found not viable
for the input side in H013.
