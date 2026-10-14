// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerCloseMetaFrame.h"
#include "crypto/SHA.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/Config.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionMeta.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
LedgerCloseMetaFrame::LedgerCloseMetaFrame(uint32_t protocolVersion,
                                           Config const& config)
{
    // The LedgerCloseMeta v() switch can be in 4 positions, 0, 1, 2 and 3. We
    // currently support all of these cases, depending on both compile time
    // and runtime conditions.
    mVersion = 0;

    if (config.USE_LEDGER_CLOSE_META_V3 &&
        protocolVersionStartsFrom(protocolVersion, ProtocolVersion::V_29))
    {
        mVersion = 3;
    }
    else if (protocolVersionStartsFrom(protocolVersion,
                                       PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
    {
        mVersion = 2;
    }
    else if (protocolVersionStartsFrom(protocolVersion,
                                       SOROBAN_PROTOCOL_VERSION))
    {
        mVersion = 1;
    }
    mLedgerCloseMeta.v(mVersion);
}

LedgerHeaderHistoryEntry&
LedgerCloseMetaFrame::ledgerHeader()
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().ledgerHeader;
    case 1:
        return mLedgerCloseMeta.v1().ledgerHeader;
    case 2:
        return mLedgerCloseMeta.v2().ledgerHeader;
    case 3:
        return mLedgerCloseMeta.v3().ledgerHeader;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::reserveTxProcessing(size_t n)
{
    switch (mVersion)
    {
    case 0:
        mLedgerCloseMeta.v0().txProcessing.reserve(n);
        break;
    case 1:
        mLedgerCloseMeta.v1().txProcessing.reserve(n);
        break;
    case 2:
        mLedgerCloseMeta.v2().txProcessing.reserve(n);
        break;
    case 3:
        mLedgerCloseMeta.v3().txProcessing.reserve(n);
        break;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::pushTxFeeProcessing(
    LedgerEntryChanges const& feeProcessing, TransactionFrameBase const& tx,
    std::optional<int64_t> baseFee)
{
    switch (mVersion)
    {
    case 0:
        mLedgerCloseMeta.v0().txProcessing.emplace_back();
        mLedgerCloseMeta.v0().txProcessing.back().feeProcessing = feeProcessing;
        break;
    case 1:
        mLedgerCloseMeta.v1().txProcessing.emplace_back();
        mLedgerCloseMeta.v1().txProcessing.back().feeProcessing = feeProcessing;
        break;
    case 2:
        mLedgerCloseMeta.v2().txProcessing.emplace_back();
        mLedgerCloseMeta.v2().txProcessing.back().feeProcessing = feeProcessing;
        break;
    case 3:
    {
        auto& txp = mLedgerCloseMeta.v3().txProcessing.emplace_back();
        txp.tx = tx.getEnvelope();
        if (baseFee)
        {
            txp.baseFee.activate() = *baseFee;
        }
        txp.feeProcessing = feeProcessing;
    }
    break;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::setTxProcessingMetaAndResultPair(
    TransactionMeta&& tm, TransactionResultPair&& rp, int index)
{
    switch (mVersion)
    {
    case 0:
    {
        auto& txp = mLedgerCloseMeta.v0().txProcessing.at(index);
        txp.txApplyProcessing = std::move(tm);
        txp.result = std::move(rp);
    }
    break;
    case 1:
    {
        auto& txp = mLedgerCloseMeta.v1().txProcessing.at(index);
        txp.txApplyProcessing = std::move(tm);
        txp.result = std::move(rp);
    }
    break;
    case 2:
    {
        auto& txp = mLedgerCloseMeta.v2().txProcessing.at(index);
        txp.txApplyProcessing = std::move(tm);
        txp.result = std::move(rp);
    }
    break;
    case 3:
    {
        // V3 meta contains the transaction envelope, so the transaction hash
        // is redundant and only the result itself is stored.
        auto& txp = mLedgerCloseMeta.v3().txProcessing.at(index);
        txp.txApplyProcessing = std::move(tm);
        txp.result = std::move(rp.result);
    }
    break;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::setPostTxApplyFeeProcessing(LedgerEntryChanges&& changes,
                                                  int index)
{
    switch (mVersion)
    {
    case 2:
        mLedgerCloseMeta.v2().txProcessing.at(index).postTxApplyFeeProcessing =
            std::move(changes);
        break;
    case 3:
        mLedgerCloseMeta.v3().txProcessing.at(index).postTxApplyFeeProcessing =
            std::move(changes);
        break;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<UpgradeEntryMeta>&
LedgerCloseMetaFrame::upgradesProcessing()
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().upgradesProcessing;
    case 1:
        return mLedgerCloseMeta.v1().upgradesProcessing;
    case 2:
        return mLedgerCloseMeta.v2().upgradesProcessing;
    case 3:
        return mLedgerCloseMeta.v3().upgradesProcessing;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::populateTxSet(TxSetXDRFrame const& txSet)
{
    switch (mVersion)
    {
    case 0:
        txSet.toXDR(mLedgerCloseMeta.v0().txSet);
        break;
    case 1:
        txSet.toXDR(mLedgerCloseMeta.v1().txSet);
        break;
    case 2:
        txSet.toXDR(mLedgerCloseMeta.v2().txSet);
        break;
    case 3:
        // V3 meta doesn't contain the transaction set, the relevant
        // per-transaction data is populated in `pushTxFeeProcessing`
        // instead.
        break;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::populateEvictedEntries(
    EvictedStateVectors const& evictedState)
{
    xdr::xvector<LedgerKey>* evictedKeysPtr = nullptr;
    switch (mVersion)
    {
    case 1:
        evictedKeysPtr = &mLedgerCloseMeta.v1().evictedKeys;
        break;
    case 2:
        evictedKeysPtr = &mLedgerCloseMeta.v2().evictedKeys;
        break;
    case 3:
        evictedKeysPtr = &mLedgerCloseMeta.v3().evictedKeys;
        break;
    default:
        releaseAssert(false);
    }
    auto& evictedKeys = *evictedKeysPtr;

    for (auto const& key : evictedState.deletedKeys)
    {
        releaseAssertOrThrow(isTemporaryEntry(key) || key.type() == TTL);
        evictedKeys.emplace_back(key);
    }
    for (auto const& entry : evictedState.archivedEntries)
    {
        releaseAssertOrThrow(isPersistentEntry(entry.data));
        evictedKeys.emplace_back(LedgerEntryKey(entry));
    }
}

void
LedgerCloseMetaFrame::setNetworkConfiguration(
    SorobanNetworkConfig const& networkConfig, bool emitExtV1)
{
    LedgerCloseMetaExt* ext = nullptr;
    switch (mVersion)
    {
    case 1:
        mLedgerCloseMeta.v1().totalByteSizeOfLiveSorobanState =
            networkConfig.getAverageSorobanStateSize();
        ext = &mLedgerCloseMeta.v1().ext;
        break;
    case 2:
        mLedgerCloseMeta.v2().totalByteSizeOfLiveSorobanState =
            networkConfig.getAverageSorobanStateSize();
        ext = &mLedgerCloseMeta.v2().ext;
        break;
    case 3:
        // V3 meta always contains the write fee, so `emitExtV1` is not
        // relevant.
        mLedgerCloseMeta.v3().totalByteSizeOfLiveSorobanState =
            networkConfig.getAverageSorobanStateSize();
        mLedgerCloseMeta.v3().sorobanFeeWrite1KB = networkConfig.feeRent1KB();
        return;
    default:
        releaseAssert(false);
    }

    if (emitExtV1)
    {
        ext->v(1);
        ext->v1().sorobanFeeWrite1KB = networkConfig.feeRent1KB();
    }
}

LedgerCloseMeta const&
LedgerCloseMetaFrame::getXDR() const
{
    return mLedgerCloseMeta;
}

#ifdef BUILD_TESTS
LedgerCloseMetaFrame::LedgerCloseMetaFrame(LedgerCloseMeta const& lm)
    : mLedgerCloseMeta(lm)
{
    mVersion = lm.v();
    if (mVersion == 1)
    {
        // This field should always be empty and never
        // used. The field only exists for legacy
        // reasons.
        releaseAssert(mLedgerCloseMeta.v1().unused.empty());
    }
}

LedgerHeader const&
LedgerCloseMetaFrame::getLedgerHeader() const
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().ledgerHeader.header;
    case 1:
        return mLedgerCloseMeta.v1().ledgerHeader.header;
    case 2:
        return mLedgerCloseMeta.v2().ledgerHeader.header;
    case 3:
        return mLedgerCloseMeta.v3().ledgerHeader.header;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<LedgerKey> const&
LedgerCloseMetaFrame::getEvictedKeys() const
{
    switch (mVersion)
    {
    case 1:
        return mLedgerCloseMeta.v1().evictedKeys;
    case 2:
        return mLedgerCloseMeta.v2().evictedKeys;
    case 3:
        return mLedgerCloseMeta.v3().evictedKeys;
    default:
        releaseAssert(false);
    }
}

size_t
LedgerCloseMetaFrame::getTransactionResultMetaCount() const
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().txProcessing.size();
    case 1:
        return mLedgerCloseMeta.v1().txProcessing.size();
    case 2:
        return mLedgerCloseMeta.v2().txProcessing.size();
    case 3:
        return mLedgerCloseMeta.v3().txProcessing.size();
    default:
        releaseAssert(false);
    }
}

TransactionMeta const&
LedgerCloseMetaFrame::getTransactionMeta(size_t index) const
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().txProcessing.at(index).txApplyProcessing;
    case 1:
        return mLedgerCloseMeta.v1().txProcessing.at(index).txApplyProcessing;
    case 2:
        return mLedgerCloseMeta.v2().txProcessing.at(index).txApplyProcessing;
    case 3:
        return mLedgerCloseMeta.v3().txProcessing.at(index).txApplyProcessing;
    default:
        releaseAssert(false);
    }
}

LedgerEntryChanges const&
LedgerCloseMetaFrame::getPreTxApplyFeeProcessing(size_t index) const
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().txProcessing.at(index).feeProcessing;
    case 1:
        return mLedgerCloseMeta.v1().txProcessing.at(index).feeProcessing;
    case 2:
        return mLedgerCloseMeta.v2().txProcessing.at(index).feeProcessing;
    case 3:
        return mLedgerCloseMeta.v3().txProcessing.at(index).feeProcessing;
    default:
        releaseAssert(false);
    }
}

LedgerEntryChanges const&
LedgerCloseMetaFrame::getPostTxApplyFeeProcessing(size_t index) const
{
    switch (mVersion)
    {
    case 2:
        return mLedgerCloseMeta.v2()
            .txProcessing.at(index)
            .postTxApplyFeeProcessing;
    case 3:
        return mLedgerCloseMeta.v3()
            .txProcessing.at(index)
            .postTxApplyFeeProcessing;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::sortTxMetaByHash()
{
    auto sortTxMeta = [](auto& txProcessing) {
        std::sort(txProcessing.begin(), txProcessing.end(),
                  [](auto const& a, auto const& b) {
                      return a.result.transactionHash <
                             b.result.transactionHash;
                  });
    };
    switch (mVersion)
    {
    case 0:
        sortTxMeta(mLedgerCloseMeta.v0().txProcessing);
        break;
    case 1:
        sortTxMeta(mLedgerCloseMeta.v1().txProcessing);
        break;
    case 2:
        sortTxMeta(mLedgerCloseMeta.v2().txProcessing);
        break;
    case 3:
    {
        // V3 meta doesn't contain the transaction hashes, so we sort by the
        // full envelope hash instead.
        auto& txProcessing = mLedgerCloseMeta.v3().txProcessing;
        std::vector<std::pair<Hash, TransactionResultMetaV2>> hashed;
        hashed.reserve(txProcessing.size());
        for (auto& txp : txProcessing)
        {
            hashed.emplace_back(xdrSha256(txp.tx), std::move(txp));
        }
        std::sort(
            hashed.begin(), hashed.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });
        for (size_t i = 0; i < hashed.size(); ++i)
        {
            txProcessing[i] = std::move(hashed[i].second);
        }
    }
    break;
    default:
        releaseAssert(false);
    }
}

#endif
}
