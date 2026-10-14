// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/TxSetFrame.h"
#include "xdr/Stellar-ledger.h"
#include <optional>

namespace stellar
{
class Config;
class TransactionFrameBase;

// Wrapper around LedgerCloseMeta XDR that provides mutable access to fields
// in the proper version of meta.
class LedgerCloseMetaFrame
{
  public:
    // Creates meta of the version appropriate for the provided protocol
    // version and config.
    LedgerCloseMetaFrame(uint32_t protocolVersion, Config const& config);

    LedgerHeaderHistoryEntry& ledgerHeader();
    void reserveTxProcessing(size_t n);
    // Adds a new transaction entry to the meta with the provided fee
    // processing changes. This has to be called for the transactions in apply
    // order.
    // `tx` and `baseFee` (the base fee that the transaction set applies to
    // `tx`) are only used for the meta versions that store them (V3+).
    void pushTxFeeProcessing(LedgerEntryChanges const& feeProcessing,
                             TransactionFrameBase const& tx,
                             std::optional<int64_t> baseFee);

    void setTxProcessingMetaAndResultPair(TransactionMeta&& tm,
                                          TransactionResultPair&& rp,
                                          int index);
    void setPostTxApplyFeeProcessing(LedgerEntryChanges&& changes, int index);

    xdr::xvector<UpgradeEntryMeta>& upgradesProcessing();

    // Stores the transaction set in meta. This is a no-op for the meta
    // versions that don't contain the transaction set (V3+).
    void populateTxSet(TxSetXDRFrame const& txSet);

    // Used for populating meta from background eviction scan
    void populateEvictedEntries(EvictedStateVectors const& evictedState);

    void setNetworkConfiguration(SorobanNetworkConfig const& networkConfig,
                                 bool emitExtV1);

    LedgerCloseMeta const& getXDR() const;

#ifdef BUILD_TESTS
    LedgerCloseMetaFrame(LedgerCloseMeta const& lm);
    LedgerHeader const& getLedgerHeader() const;
    xdr::xvector<LedgerKey> const& getEvictedKeys() const;
    size_t getTransactionResultMetaCount() const;
    TransactionMeta const& getTransactionMeta(size_t index) const;
    LedgerEntryChanges const& getPreTxApplyFeeProcessing(size_t index) const;
    LedgerEntryChanges const& getPostTxApplyFeeProcessing(size_t index) const;
    void sortTxMetaByHash();
#endif

  private:
    LedgerCloseMeta mLedgerCloseMeta;
    int mVersion;
};

}
