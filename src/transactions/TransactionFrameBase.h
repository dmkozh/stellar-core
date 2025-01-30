// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <optional>

#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerStateSnapshot.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionMetaFrame.h"
#include "util/TxResource.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include <optional>

#include "ledger/SorobanMetrics.h"

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class Database;
class OperationFrame;
class TransactionFrame;
class FeeBumpTransactionFrame;
class AppConnector;

class MutableTransactionResultBase;
using MutableTxResultPtr = std::shared_ptr<MutableTransactionResultBase>;

class TransactionFrameBase;
using TransactionFrameBasePtr = std::shared_ptr<TransactionFrameBase const>;
using TransactionFrameBaseConstPtr =
    std::shared_ptr<TransactionFrameBase const>;

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

using ModifiedEntryMap = UnorderedMap<LedgerKey, std::optional<LedgerEntry>>;

struct ThreadEntry
{
    // Will not be set if the entry doesn't exist, or if no tx was able to load
    // it due to hitting read limits.
    std::optional<LedgerEntry> mLedgerEntry;
    bool isDirty;
};

using ThreadEntryMap = UnorderedMap<LedgerKey, ThreadEntry>;

struct ParallelTxReturnVal
{
    bool mSuccess{false};
    // This will contain a key for every entry modified by a transaction
    ModifiedEntryMap mModifiedEntryMap;
};

class ParallelLedgerInfo
{

  public:
    ParallelLedgerInfo(uint32_t version, uint32_t seq, uint32_t reserve,
                       TimePoint time, Hash id)
        : ledgerVersion(version)
        , ledgerSeq(seq)
        , baseReserve(reserve)
        , closeTime(time)
        , networkID(id)
    {
    }

    uint32_t
    getLedgerVersion() const
    {
        return ledgerVersion;
    }
    uint32_t
    getLedgerSeq() const
    {
        return ledgerSeq;
    }
    uint32_t
    getBaseReserve() const
    {
        return baseReserve;
    }
    TimePoint
    getCloseTime() const
    {
        return closeTime;
    }
    Hash
    getNetworkID() const
    {
        return networkID;
    }

  private:
    uint32_t ledgerVersion;
    uint32_t ledgerSeq;
    uint32_t baseReserve;
    TimePoint closeTime;
    Hash networkID;
};

class TxEffects
{
  public:
    TxEffects(uint32_t ledgerVersion) : mMeta(ledgerVersion)
    {
    }

    TransactionMetaFrame&
    getMeta()
    {
        return mMeta;
    }
    LedgerTxnDelta&
    getDelta()
    {
        return mDelta;
    }

  private:
    TransactionMetaFrame mMeta;
    LedgerTxnDelta mDelta;
};

class TxBundle
{
  public:
    TxBundle(TransactionFrameBasePtr tx, MutableTxResultPtr resPayload,
             uint32_t ledgerVersion, uint64_t txNum)
        : tx(tx)
        , resPayload(resPayload)
        , txNum(txNum)
        , mEffects(new TxEffects(ledgerVersion))
    {
    }

    TransactionFrameBasePtr
    getTx() const
    {
        return tx;
    }
    MutableTxResultPtr
    getResPayload() const
    {
        return resPayload;
    }
    uint64_t
    getTxNum() const
    {
        return txNum;
    }
    TxEffects&
    getEffects() const
    {
        return *mEffects;
    }

  private:
    TransactionFrameBasePtr tx;
    MutableTxResultPtr resPayload;
    uint64_t txNum;
    std::unique_ptr<TxEffects> mEffects;
};

typedef std::vector<TxBundle> Thread;
typedef std::vector<Thread> ApplyStage;
typedef UnorderedMap<LedgerKey, TTLEntry> TTLs;

#endif

class TransactionFrameBase
{
  public:
    static TransactionFrameBasePtr
    makeTransactionFromWire(Hash const& networkID,
                            TransactionEnvelope const& env);

    virtual bool apply(AppConnector& app, AbstractLedgerTxn& ltx,
                       TransactionMetaFrame& meta, MutableTxResultPtr txResult,
                       Hash const& sorobanBasePrngSeed = Hash{}) const = 0;

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    virtual void preParallelApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                  TransactionMetaFrame& meta,
                                  MutableTxResultPtr resPayload) const = 0;

    virtual ParallelTxReturnVal parallelApply(
        ThreadEntryMap const& entryMap, // Must not be shared between threads!,
        Config const& config, SorobanNetworkConfig const& sorobanConfig,
        ParallelLedgerInfo const& ledgerInfo, MutableTxResultPtr resPayload,
        SorobanMetrics& sorobanMetrics, Hash const& sorobanBasePrngSeed,
        TxEffects& effects) const = 0;
#endif

    virtual MutableTxResultPtr
    checkValid(AppConnector& app, LedgerSnapshot const& ls,
               SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset) const = 0;
    virtual bool checkSorobanResourceAndSetError(
        AppConnector& app, SorobanNetworkConfig const& cfg,
        uint32_t ledgerVersion, MutableTxResultPtr txResult) const = 0;

    virtual MutableTxResultPtr createSuccessResult() const = 0;

    virtual MutableTxResultPtr
    createSuccessResultWithFeeCharged(LedgerHeader const& header,
                                      std::optional<int64_t> baseFee,
                                      bool applying) const = 0;

    virtual TransactionEnvelope const& getEnvelope() const = 0;

#ifdef BUILD_TESTS
    virtual TransactionEnvelope& getMutableEnvelope() const = 0;
    virtual void clearCached() const = 0;
    virtual bool isTestTx() const = 0;
#endif

    // Returns the total fee of this transaction, including the 'flat',
    // non-market part.
    virtual int64_t getFullFee() const = 0;
    // Returns the part of the full fee used to make decisions as to
    // whether this transaction should be included into ledger.
    virtual int64_t getInclusionFee() const = 0;
    virtual int64_t getFee(LedgerHeader const& header,
                           std::optional<int64_t> baseFee,
                           bool applying) const = 0;

    virtual Hash const& getContentsHash() const = 0;
    virtual Hash const& getFullHash() const = 0;

    virtual uint32_t getNumOperations() const = 0;
    virtual Resource getResources(bool useByteLimitInClassic) const = 0;

    virtual std::vector<Operation> const& getRawOperations() const = 0;

    virtual SequenceNumber getSeqNum() const = 0;
    virtual AccountID getFeeSourceID() const = 0;
    virtual AccountID getSourceID() const = 0;
    virtual std::optional<SequenceNumber const> const getMinSeqNum() const = 0;
    virtual Duration getMinSeqAge() const = 0;
    virtual uint32 getMinSeqLedgerGap() const = 0;

    virtual void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const = 0;
    virtual void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys,
                                      LedgerKeyMeter* lkMeter) const = 0;

    virtual MutableTxResultPtr
    processFeeSeqNum(AbstractLedgerTxn& ltx,
                     std::optional<int64_t> baseFee) const = 0;

    virtual void processPostApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                  TransactionMetaFrame& meta,
                                  MutableTxResultPtr txResult) const = 0;

    virtual std::shared_ptr<StellarMessage const> toStellarMessage() const = 0;

    virtual bool hasDexOperations() const = 0;

    virtual bool isSoroban() const = 0;
    virtual SorobanResources const& sorobanResources() const = 0;
    virtual int64 declaredSorobanResourceFee() const = 0;
    virtual bool XDRProvidesValidFee() const = 0;
};
}
