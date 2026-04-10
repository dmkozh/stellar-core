# H003: LastClosedLedgerHeader Pays Unnecessary Base64 Encode/Decode Overhead

**Date**: 2026-04-10
**Subsystem**: database, ledger
**Severity**: Informational
**Impact**: CPU + write amplification
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The per-ledger restart copy of the last closed ledger header should be stored in
a compact binary form aligned with its XDR representation. The database hot
path should not base64-expand the header just to fit a generic TEXT storage
path, then reverse that expansion again on startup.

## Mechanism

`LedgerHeaderUtils::encodeHeader()` first materializes opaque XDR bytes and then
base64-encodes them before `setMainState(kLastClosedLedgerHeader, ...)` writes
the result to `storestate.state TEXT`. On restart,
`LedgerHeaderUtils::decodeFromData()` base64-decodes the string back into bytes.
Persisting the header as raw opaque bytes (or a dedicated compact schema) would
remove the extra encode/decode pass and the base64 size inflation from every
timed ledger close.

## Trigger

Run any apply-load benchmark scenario with write timing enabled. Every ledger
close encodes and writes `kLastClosedLedgerHeader`, and every restart decodes it
before rebuilding ledger state.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:storePersistentStateAndLedgerHeaderInDB:2938-2940` — per-ledger DB write of the encoded header
- `src/ledger/LedgerHeaderUtils.cpp:encodeHeader/decodeFromData:40-59,103-125` — current path converts XDR bytes to/from base64 text
- `src/main/PersistentState.cpp:updateDb:281-318` — stores the base64 text in the generic `storestate` table
- `src/ledger/LedgerManagerImpl.cpp:loadLastKnownLedgerInternal:543-553` — restart path decodes the stored string back into a `LedgerHeader`

## Evidence

The encode path already creates the raw XDR byte vector via
`xdr::xdr_to_opaque(header)` before immediately converting it to base64 for DB
storage. The decode path undoes exactly that transformation on restart, so the
extra work is purely an artifact of funneling the header through a TEXT-valued
key/value table.

## Anti-Evidence

The header payload is materially smaller than `HistoryArchiveState`, so the
absolute gain here is probably bounded. Any change also needs a migration story
for existing `storestate` rows and a backend-safe way to distinguish binary from
legacy text data.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (distinct from fail/005 which proposes skipping writes entirely, and fail/014 which targets HAS JSON format)
**Failed At**: reviewer

### Trace Summary

Traced the full encode/decode path: `storePersistentStateAndLedgerHeaderInDB` (line 2938) calls `LedgerHeaderUtils::encodeHeader` which calls `xdr::xdr_to_opaque(header)` (~280-400 bytes for a LedgerHeader) then `decoder::encode_b64` (a character-by-character base64 transform producing ~370-530 bytes). The result is stored via `PersistentState::updateDb` as a SQL UPDATE on the `storestate` TEXT column. On restart, `decodeFromData` reverses this with `decoder::decode_b64` then XDR deserialization.

### Code Paths Examined

- `src/ledger/LedgerHeaderUtils.cpp:encodeHeader:40-53` — `xdr_to_opaque` produces ~300 bytes, then `encode_b64` adds ~33% overhead. Both are trivial operations on sub-KB data.
- `src/ledger/LedgerHeaderUtils.cpp:decodeFromData:103-125` — Mirror decode, equally trivial.
- `src/ledger/LedgerManagerImpl.cpp:storePersistentStateAndLedgerHeaderInDB:2901-2948` — The base64 header encode (line 2938) sits alongside `xdrSha256(header)` (line 2906), `getLiveBucketList()` copy (line 2916), `HistoryArchiveState` construction (lines 2920-2933), `has.toString()` JSON serialization (line 2936), and two SQL UPDATE round-trips (lines 2935-2940). Each of these sibling operations is orders of magnitude more expensive than the base64 encode of ~300 bytes.
- `src/util/Decoder.h:encode_b64:37-45` — Simple template wrapping `bn::encode_b64`, iterates input character by character. No allocation beyond the output string.

### Why It Failed

The inefficiency technically exists but is **below the noise floor**. A LedgerHeader serializes to approximately 280-400 bytes of XDR. Base64 encoding this data takes on the order of 100ns-1µs — negligible compared to the surrounding operations in the same function:

1. **`xdrSha256(header)`** — SHA-256 hash computation
2. **`getLiveBucketList()`** — copies the entire BucketList structure
3. **`HistoryArchiveState` construction** — iterates all ~22 BucketList levels
4. **`has.toString()`** — full JSON serialization via cereal of the HAS (~5-10KB of JSON)
5. **Two SQL UPDATE statements** — actual disk I/O with WAL journaling

Even in the most generous estimate, eliminating the base64 encode/decode saves <0.001% of a ledger close that takes hundreds of milliseconds to seconds. The 33% size inflation (from ~300 to ~400 bytes) is immaterial when the adjacent HAS JSON write is 10-30× larger. This cannot produce a measurable improvement in any benchmark scenario.

### Lesson Learned

LedgerHeader is a small fixed-size structure (~300 bytes XDR). Encoding optimizations on sub-KB payloads that execute once per ledger cannot produce measurable benchmark improvements. Focus optimization effort on operations proportional to transaction count or entry count, not per-ledger fixed costs on tiny structures.
