# H005: Erase-Emplace Pattern in InMemorySorobanState::updateContractData Causes Redundant Rehashing

**Date**: 2025-07-22
**Subsystem**: storage (ledger)
**Severity**: Low
**Impact**: per-entry CPU overhead during in-memory Soroban state update
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When updating a CONTRACT_DATA entry in the in-memory Soroban state, only the
entry's value fields (the `LedgerEntry` data and accounting sizes) should
change. The entry's position in the hash table should remain stable since the
key (`LedgerKey`) does not change during an update. The hash table should
not need to rehash the key or reallocate the node.

## Mechanism

`InMemorySorobanState::updateContractData` (InMemorySorobanState.cpp:92-111)
uses an erase+emplace pattern to update entries in `mContractDataEntries`
(an `unordered_set<InternalContractDataMapEntry>`):

1. `find` the entry — hashes the lookup key (line 98)
2. `erase` the entry — deallocates the hash table node (line 108)
3. `emplace` a new entry — hashes the new entry's key, allocates a new node
   with `make_shared<BucketEntry const>` (line 109-110)

Since `unordered_set` elements are logically const (the element IS the key),
in-place modification is not supported. The erase+emplace forces:
- 2 hash computations: one for erase (find internally), one for emplace
  insert. Each hash involves `getTTLKey` which computes SHA256
  (`sha256(xdr_to_opaque(e))`)
- 1 node deallocation + 1 node allocation + 1 `make_shared` heap allocation

For ~8,000-16,000 CONTRACT_DATA updates per ledger (SAC benchmark), with
SHA256 at ~1μs per call: ~16,000-32,000μs = ~16-32ms total for hash
computations alone. However, the `find` at line 98 ALSO computes a hash,
making it 3 hash computations per update, not 2.

## Trigger

Every Soroban ledger close with CONTRACT_DATA modifications. SAC benchmark
with 3200 txs × ~2-3 CONTRACT_DATA updates each ≈ ~6,400-9,600 updates.

## Target Code

- `src/ledger/InMemorySorobanState.cpp:updateContractData:92-111` — erase+emplace pattern
- `src/ledger/InMemorySorobanState.h:InternalContractDataMapEntry:101-170` — the set element type
- `src/ledger/InMemorySorobanState.h:ValueEntry::hash:127-132` — calls `getTTLKey` → SHA256
- `src/ledger/LedgerTypeUtils.cpp:getTTLKey:31-38` — `sha256(xdr_to_opaque(e))` per call

## Evidence

1. The erase+emplace pattern is clearly visible at lines 108-110.
2. `InternalContractDataMapEntry::ValueEntry::hash()` at line 127-132 calls
   `getTTLKey` which invokes `sha256(xdr_to_opaque(entry.contractData()))`.
3. Each `unordered_set` operation (find, erase rehash, emplace insert) triggers
   the hash function on the element.
4. Total SHA256 cost per update: ~3μs (3 hash calls × ~1μs each).

## Anti-Evidence

1. The TTL key hash caching approach was already investigated (ledger/reviewed/001)
   and a PoC attempt (transaction-ledger/fail/010) showed that caching TTL keys
   in an `unordered_map<LedgerKey, LedgerKey>` was a NET REGRESSION because the
   cache's own hash computation (hashing complex `LedgerKey` objects) exceeded
   the SHA256 savings.
2. Switching from `unordered_set` to `unordered_map` with a stable key would
   require the same expensive `LedgerKey` hashing that defeated the caching
   approach in fail/010.
3. The `InternalContractDataMapEntry` design uses C++20 heterogeneous lookup
   with type-erased keys specifically to optimize the find path — the `QueryKey`
   subclass pre-computes the hash once. Restructuring to `unordered_map` would
   lose this optimization.
4. The SHA256 computation in `getTTLKey` operates on small inputs (~50-100
   bytes of XDR-serialized contract key), which modern CPUs with SHA extensions
   can compute in ~200-500ns, not the ~1μs estimated.
5. With hardware SHA256: ~9,600 updates × 3 hashes × 300ns = ~8.6ms. Against
   100-500ms ledger close: ~1.7-8.6%. While potentially measurable, the known
   constraint from fail/010 means alternative data structure approaches also
   add comparable overhead.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-22
**Failed At**: hypothesis
**Novelty**: PARTIAL — the erase+emplace pattern itself is novel, but the
underlying SHA256 cost is well-studied in ledger/reviewed/001 and
transaction-ledger/fail/010. The key insight from fail/010 (that alternative
hash-based caching structures have comparable overhead to SHA256) applies
directly here.

### Why It Failed

The erase+emplace pattern is a consequence of using `unordered_set` where
elements serve as both key and value. Switching to `unordered_map` (the
natural fix for in-place updates) would require hashing `LedgerKey` objects
for the map's key, which was demonstrated in transaction-ledger/fail/010 to
be at least as expensive as the SHA256 computation it would eliminate. The
`InternalContractDataMapEntry` design with its `QueryKey` pre-computed hash
and heterogeneous lookup is already an optimization of this hash cost — the
current erase+emplace is the cost-minimizing approach given the constraint
that `LedgerKey` hashing is expensive.

The theoretical fix would require a fundamentally cheaper key representation
(e.g., integer IDs for contract entries), which is a protocol-level change
outside the scope of C++ optimization.

### Lesson Learned

When an `unordered_set` uses a computationally expensive hash function, the
erase+emplace anti-pattern has 3× the hash cost of a single lookup. However,
if the fix (switching to `unordered_map`) requires equally expensive hashing
for the map key, the net improvement is zero. Always check that the
alternative data structure's hash function is cheaper before proposing a
container change.
