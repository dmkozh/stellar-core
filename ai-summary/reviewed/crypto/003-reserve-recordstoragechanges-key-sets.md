# H003: Reserve `recordStorageChanges()` Key Sets Before Decoding Large Host Outputs

**Date**: 2026-04-10
**Subsystem**: crypto, transactions
**Severity**: Medium
**Impact**: post-host rehashing of expensive `LedgerKey`s during storage-change reconciliation
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once Rust has returned the number of modified ledger entries for a host
invocation, the C++ post-processing path should pre-size its temporary key sets
from that count before inserting decoded keys. Large SAC outputs should not
force `recordStorageChanges()` to grow and rehash `createdAndModifiedKeys` and
`createdKeys` while it is already paying to decode every entry.

## Mechanism

`recordStorageChanges()` creates two empty `UnorderedSet<LedgerKey>` instances
and then inserts every returned key into `createdAndModifiedKeys`, and inserts
again into `createdKeys` when `upsertLedgerEntry()` reports a logical create.
The batched SAC benchmark generates fresh destination contract addresses per tx,
so each 100-destination transfer tends to create around 100 new balance entries
plus their TTL entries, producing a large `createdKeys` set in addition to the
full modified-entry set. Growing both sets from zero triggers repeated rehashes
of `CONTRACT_DATA` balance keys exactly where the hot path has just decoded them
from host output.

## Trigger

Run SAC apply-load with batched transfers and profile `recordStorageChanges()`,
`unordered_set` growth, and `std::hash<LedgerKey>` after host invocation.
Compare against a build that reserves `createdAndModifiedKeys` from
`out.modified_ledger_entries.size()` and reserves `createdKeys` from an estimate
based on the write footprint / modified-entry count before the decode loop.

## Target Code

- `docs/apply-load-benchmark-sac.cfg:31-38` — SAC benchmark uses a 100-destination batch size
- `src/simulation/ApplyLoad.cpp:2094-2103` — batched SAC destinations are intentionally unique per tx
- `src/simulation/TxGenerator.cpp:1492-1512` — each unique destination becomes a distinct `Balance` `CONTRACT_DATA` key in the write footprint
- `src/transactions/InvokeHostFunctionOpFrame.cpp:610-703` — `recordStorageChanges()` builds two empty key sets, inserts decoded keys, then probes them again during deletion inference
- `src/transactions/InvokeHostFunctionOpFrame.cpp:655-658` — `createdKeys` grows on every logical create
- `src/ledger/LedgerHashUtils.h:178-184` — hashing each `Balance` contract-data key requires `shortHash::xdrComputeHash` over the SCVal key

## Evidence

The benchmarked SAC path deliberately creates fresh destination addresses, so
the post-host reconciliation loop does not just update existing keys — it often
creates many new `Balance` and TTL entries in the same tx. Both temporary sets
start at zero capacity even though the code already knows the exact modified
entry count before the first insertion. That combination makes repeated
rehashing avoidable on a very regular benchmark workload.

## Anti-Evidence

`custom_token` transfers update existing balances and have much smaller outputs,
so `createdKeys` is not large there. This hypothesis is therefore aimed at the
batched SAC scenario first; the other models may show only marginal gains.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced `recordStorageChanges()` in `InvokeHostFunctionOpFrame.cpp:610-703`. Two `UnorderedSet<LedgerKey>` instances (which are `std::unordered_set` with `RandHasher`) are created empty and populated from `out.modified_ledger_entries`. For the SAC batch benchmark with `APPLY_LOAD_BATCH_SAC_COUNT=100`, each tx creates ~100 new Balance `CONTRACT_DATA` entries + ~100 TTL entries + a few modified source entries, totaling ~202 modified entries. Growing both sets from zero bucket count triggers ~8 rehash events each, producing ~186 extra hash calls per set. However, the absolute cost is very small relative to ledger close time.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:614-615` — Two empty `UnorderedSet<LedgerKey>` created with default (zero or minimal) bucket count
- `src/transactions/InvokeHostFunctionOpFrame.cpp:616-659` — Loop inserts ~202 entries into `createdAndModifiedKeys`, ~200 into `createdKeys`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:689-702` — Deletion loop probes `createdAndModifiedKeys` for each read-write footprint entry (lookups only, no rehash cost)
- `src/util/UnorderedSet.h:13` — `UnorderedSet` is `std::unordered_set<KeyT, RandHasher<KeyT, Hasher>>`
- `src/util/RandHasher.h:20-29` — `RandHasher` delegates to `std::hash<LedgerKey>` then XORs with `gMixer`
- `src/ledger/LedgerHashUtils.h:178-185` — `CONTRACT_DATA` hash calls `shortHash::xdrComputeHash(lk.contractData().key)` for the SCVal key + `std::hash<SCAddress>` for the contract address
- `src/crypto/ShortHash.cpp:74-79` — `XDRShortHasher` constructor acquires `gKeyMutex` to copy key (~10-20ns, uncontended in T=1; potentially contended in T=8)
- `src/crypto/ShortHash.h:47-55` — `xdrComputeHash` creates an `XDRShortHasher`, walks the XDR structure via SipHash, then returns digest
- `src/simulation/ApplyLoad.cpp:2070-2103` — SAC benchmark creates 100 unique destinations per tx, each becoming a distinct `Balance` `CONTRACT_DATA` entry

### Findings

**The inefficiency is real.** Both `createdAndModifiedKeys` and `createdKeys` start at default capacity despite the exact count being known from `out.modified_ledger_entries.size()`. With ~202 entries per SAC tx, each set undergoes ~8 rehash events, producing ~186 extra hash calls per set (~372 total). For `CONTRACT_DATA` keys, each extra hash involves `xdrComputeHash` on the SCVal key (SipHash-2-4 with mutex acquisition in the constructor).

**The fix is trivially correct.** Calling `createdAndModifiedKeys.reserve(out.modified_ledger_entries.size())` before the loop eliminates all rehashing. For `createdKeys`, reserving with the same count (an upper bound) is safe and effective. No ownership, thread-safety, or API contract issues.

**The impact is too small for measurable benchmark improvement.** The extra rehash cost per tx is approximately:
- ~186 extra `CONTRACT_DATA` key hashes × ~125ns each = ~23µs
- ~186 extra `TTL` key hashes × ~15ns each = ~3µs
- ~8 bucket-array reallocations × ~500ns each = ~4µs
- Total: ~30µs per tx

With 32 SAC txs per ledger (3200 transfers / 100 batch size), the total per-ledger overhead is ~960µs (~1ms). Against ledger close times of hundreds of milliseconds to seconds, this is <0.5% — well below the 5% threshold for "Low" severity.

**Context from H006 (shortHash mutex):** The related investigation into removing the shortHash global mutex entirely (a much larger optimization than avoiding a few rehash rounds) went through full benchmarking and showed no measurable improvement — in fact, the most relevant scenario regressed. This strongly suggests that hash-function overhead in this code path is not a measurable bottleneck.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp:614-615` (the two `UnorderedSet<LedgerKey>` declarations)
- **Change description**: Add `createdAndModifiedKeys.reserve(out.modified_ledger_entries.size())` and `createdKeys.reserve(out.modified_ledger_entries.size())` after the declarations (before the loop on line 616). The second reserve uses the total count as an upper bound since exact created count isn't known upfront.
- **Correctness check**: Run `[tx]` and `[soroban]` tagged tests. The reserve only affects bucket pre-allocation, not functional behavior.
- **Benchmark focus**: SAC scenario with `APPLY_LOAD_BATCH_SAC_COUNT=100`. Expected improvement: <0.5% — likely within noise floor. The optimization is correct but the absolute savings (~1ms/ledger) are too small to register against benchmark variance.
