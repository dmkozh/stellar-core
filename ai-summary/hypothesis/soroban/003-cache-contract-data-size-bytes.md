# H003: Store CONTRACT_DATA sizeBytes in InMemorySorobanState Like CONTRACT_CODE

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Low
**Impact**: CPU / repeated XDR size walks in post-close Soroban state maintenance
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The post-close in-memory Soroban state update should not need to recompute the
old XDR size of every modified `CONTRACT_DATA` entry on every ledger. Once a
contract-data entry is in `InMemorySorobanState`, its current accounted size
should be stored alongside the entry, just as contract-code entries already
store `sizeBytes`.

## Mechanism

`ContractCodeMapEntryT` caches `sizeBytes` specifically “to make the contract
code updates faster”, but `ContractDataMapEntryT` stores only the entry and TTL.
As a result, `updateContractData`, `createContractDataEntry`, and
`deleteContractData` repeatedly call `xdr::xdr_size` over old/new
contract-data entries while `updateState` walks every modified Soroban entry
after each ledger close. In write-heavy apply-load runs this creates a pure
post-host tax that scales with the number of contract-data balance / pool-state
updates rather than with useful execution.

## Trigger

Run batched SAC, `custom_token`, or `soroswap` apply-load and sample
`InMemorySorobanState::updateState`, especially `updateContractData`,
`createContractDataEntry`, and `deleteContractData`. If the hypothesis is
correct, profiles will show repeated `xdr::xdr_size` work on contract-data
entries after transaction execution has already finished.

## Target Code

- `src/ledger/InMemorySorobanState.h:46-79` — contract-data entries lack a cached `sizeBytes`, while contract-code entries already keep one
- `src/ledger/InMemorySorobanState.cpp:updateContractData:92-110` — recomputes both old and new XDR sizes on every update
- `src/ledger/InMemorySorobanState.cpp:createContractDataEntry:114-141` — recomputes XDR size on every create
- `src/ledger/InMemorySorobanState.cpp:deleteContractData:193-201` — recomputes old XDR size on every delete
- `src/ledger/InMemorySorobanState.cpp:updateState:553-597` — runs this maintenance pass on every closed ledger

## Evidence

The code already contains the precedent: contract-code state caches
`sizeBytes` because size recomputation is expensive enough to warrant stored
metadata. Contract-data updates currently lack the same optimization even
though apply-load workloads are dominated by contract-data writes, especially
batched SAC balance updates and soroswap/custom-token state mutations.

## Anti-Evidence

This does not eliminate all size work: newly written contract-data entries still
need one fresh size computation unless earlier phases thread it through.
Compared to the contract-data hash issue, this path is confined to the serial
post-close state-maintenance phase, so its ceiling is lower if host execution or
bucket writes dominate a scenario.
