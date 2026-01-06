#include <chrono>
#include <optional>
#include <type_traits>

#include "ledger/test/LedgerTestUtils.h"
#include "test/Catch2.h"
#include "util/Math.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-types.h"
#include "xdrpp/marshal.h"
#include "xdrpp/message.h"
#include "xdrpp/types.h"

namespace stellar
{
using namespace xdr;

// XDR offset finder that uses template specialization for schema compatibility
class LedgerCloseMetaOffsetFinder
{
  public:
    struct TransactionResultMetaOffset
    {
        size_t startOffset;
        size_t endOffset;
    };

    LedgerCloseMetaOffsetFinder(xdr::xvector<uint8_t> const& buffer,
                                Hash const& targetHash)
        : mBuffer(buffer), mTargetHash(targetHash)
    {
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<uint32_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T& t)
    {
        if (mFoundOffset.has_value())
        {
            return;
        }
        uint32_t v;
        getBytes(&v, 4);
        t = xdr_traits<T>::from_uint(swap32(v));
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<uint64_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T& t)
    {
        if (mFoundOffset.has_value())
        {
            return;
        }
        uint64_t v;
        getBytes(&v, 8);
        t = xdr_traits<T>::from_uint(swap64(v));
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_bytes>::type
    operator()(T& t)
    {
        if (mFoundOffset.has_value())
        {
            return;
        }
        if (xdr_traits<T>::variable_nelem)
        {
            uint32_t size;
            getBytes(&size, 4);
            size = swap32(size);
            skip(size);
        }
        else
        {
            skip(xdr_traits<T>::serial_size(t));
        }
    }

    template <typename T>
    typename std::enable_if<!std::is_same<TransactionResultPair, T>::value &&
                            !std::is_same<TransactionResultMetaV1, T>::value &&
                            !std::is_same<TransactionResultMeta, T>::value &&
                            xdr_traits<T>::is_class &&
                            !xdr_traits<T>::is_container>::type
    operator()(T& t)
    {
        if (mFoundOffset.has_value())
        {
            return;
        }
        xdr_traits<T>::load(*this, t);
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_container>::type
    operator()(T& t)
    {
        using value_type = typename xdr_traits<T>::value_type;
        if (mFoundOffset.has_value())
        {
            return;
        }
        uint32_t size;
        if (xdr_traits<T>::variable_nelem)
        {
            getBytes(&size, 4);
            size = swap32(size);
        }
        else
        {
            size = static_cast<uint32_t>(t.size());
        }
        value_type val;
        for (uint32_t i = 0; i < size; ++i)
        {
            (*this)(val);
        }
    }

    template <typename T>
    typename std::enable_if<std::is_same<TransactionResultPair, T>::value>::type
    operator()(T& pair)
    {
        if (mFoundOffset.has_value())
        {
            return;
        }
        check(32);

        if (std::memcmp(mBuffer.data() + mCurrIndex, mTargetHash.data(), 32) ==
            0)
        {
            mFound = true;
        }
        mCurrIndex += 32;
        TransactionResult result;
        (*this)(result);
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<TransactionResultMetaV1, T>::value>::type
    operator()(T& meta)
    {
        if (mFoundOffset.has_value())
        {
            return;
        }
        size_t startOffset = mCurrIndex;

        ExtensionPoint ext;
        (*this)(ext);

        TransactionResultPair result;
        size_t pairStart = mCurrIndex;
        (*this)(result);
        LedgerEntryChanges feeProcessing;
        (*this)(feeProcessing);
        TransactionMeta txApplyProcessing;
        (*this)(txApplyProcessing);
        LedgerEntryChanges postTxApplyFeeProcessing;
        (*this)(postTxApplyFeeProcessing);

        if (mFound && !mFoundOffset.has_value())
        {
            mFoundOffset = TransactionResultMetaOffset{startOffset, mCurrIndex};
        }
    }

    template <typename T>
    typename std::enable_if<std::is_same<TransactionResultMeta, T>::value>::type
    operator()(T& meta)
    {
        if (mFoundOffset.has_value())
        {
            return;
        }
        size_t startOffset = mCurrIndex;

        TransactionResultPair result;
        (*this)(result);
        LedgerEntryChanges feeProcessing;
        (*this)(feeProcessing);
        TransactionMeta txApplyProcessing;
        (*this)(txApplyProcessing);

        if (mFound && !mFoundOffset.has_value())
        {
            mFoundOffset = TransactionResultMetaOffset{startOffset, mCurrIndex};
        }
    }

  public:
    void
    check(std::size_t n) const
    {
        if (mCurrIndex + n > mBuffer.size())
        {
            throw xdr_overflow("Input buffer space overflow in "
                               "LedgerCloseMetaOffsetFinder.");
        }
    }

    void
    getBytes(void* buf, size_t len)
    {
        if (len != 0)
        {
            check(len);
            std::memcpy(buf, mBuffer.data() + mCurrIndex, len);
            mCurrIndex += len;
            while (len & 3)
            {
                ++mCurrIndex;
                ++len;
            }
        }
    }

    void
    skip(size_t n)
    {
        check(n);
        mCurrIndex += n;
        while (n & 3)
        {
            ++mCurrIndex;
            ++n;
        }
    }

    xdr::xvector<uint8_t> const& mBuffer;
    Hash const& mTargetHash;
    size_t mCurrIndex = 0;
    bool mFound = false;
    std::optional<TransactionResultMetaOffset> mFoundOffset;
};

// Function to find TransactionResultMeta byte offsets by transaction hash
std::optional<LedgerCloseMetaOffsetFinder::TransactionResultMetaOffset>
findTransactionResultMetaOffsets(
    xdr::xvector<uint8_t> const& encodedLedgerCloseMeta,
    Hash const& transactionHash)
{
    LedgerCloseMetaOffsetFinder finder(encodedLedgerCloseMeta, transactionHash);
    LedgerCloseMeta lcm;
    xdr_argpack_archive(finder, lcm);
    return finder.mFoundOffset;
}

namespace
{

// Helper function to generate random hash
Hash
generateRandomHash()
{
    Hash h;
    for (size_t i = 0; i < h.size(); ++i)
    {
        h[i] = static_cast<uint8_t>(rand_uniform<uint32_t>(0, 255));
    }
    return h;
}

// Helper function to generate random transaction result
TransactionResult
generateRandomTransactionResult()
{
    TransactionResult result{};
    result.result.code(txSUCCESS);

    // Add some random operation results
    size_t numOps = rand_uniform<size_t>(1, 5);
    result.result.results().resize(numOps);
    for (auto& opResult : result.result.results())
    {
        opResult.code(opINNER);
        opResult.tr().type(CREATE_ACCOUNT);
        opResult.tr().createAccountResult().code(CREATE_ACCOUNT_SUCCESS);
    }
    return result;
}

// Helper function to generate random ledger entry changes
LedgerEntryChanges
generateRandomLedgerEntryChanges()
{
    LedgerEntryChanges changes;
    size_t numChanges = rand_uniform<size_t>(0, 3);
    changes.resize(numChanges);

    for (auto& change : changes)
    {
        if (rand_flip())
        {
            change.type(LEDGER_ENTRY_CREATED);
            change.created() = LedgerTestUtils::generateValidLedgerEntry();
        }
        else
        {
            change.type(LEDGER_ENTRY_UPDATED);
            change.updated() = LedgerTestUtils::generateValidLedgerEntry();
        }
    }

    return changes;
}

// Helper function to generate random transaction meta
TransactionMeta
generateRandomTransactionMeta()
{
    TransactionMeta meta;

    // Randomly choose meta version
    int version = rand_uniform<int>(0, 4);
    meta.v(version);

    switch (version)
    {
    case 0:
    {
        size_t numOps = rand_uniform<size_t>(1, 3);
        meta.operations().resize(numOps);
        for (auto& opMeta : meta.operations())
        {
            opMeta.changes = generateRandomLedgerEntryChanges();
        }
        break;
    }
    case 1:
    {
        auto& v1 = meta.v1();
        v1.txChanges = generateRandomLedgerEntryChanges();
        size_t numOps = rand_uniform<size_t>(1, 3);
        v1.operations.resize(numOps);
        for (auto& opMeta : v1.operations)
        {
            opMeta.changes = generateRandomLedgerEntryChanges();
        }
        break;
    }
    case 2:
    {
        auto& v2 = meta.v2();
        v2.txChangesBefore = generateRandomLedgerEntryChanges();
        size_t numOps = rand_uniform<size_t>(1, 3);
        v2.operations.resize(numOps);
        for (auto& opMeta : v2.operations)
        {
            opMeta.changes = generateRandomLedgerEntryChanges();
        }
        v2.txChangesAfter = generateRandomLedgerEntryChanges();
        break;
    }
    case 3:
    {
        auto& v3 = meta.v3();
        v3.txChangesBefore = generateRandomLedgerEntryChanges();
        size_t numOps = rand_uniform<size_t>(1, 3);
        v3.operations.resize(numOps);
        for (auto& opMeta : v3.operations)
        {
            opMeta.changes = generateRandomLedgerEntryChanges();
        }
        v3.txChangesAfter = generateRandomLedgerEntryChanges();
        if (rand_flip())
        {
            v3.sorobanMeta.activate();
            v3.sorobanMeta->ext.v(1);
            v3.sorobanMeta->events.resize(rand_uniform<size_t>(0, 5));
            v3.sorobanMeta->returnValue.type(SCV_BYTES);
            v3.sorobanMeta->returnValue.bytes().resize(
                rand_uniform<size_t>(0, 10));
            v3.sorobanMeta->diagnosticEvents.resize(rand_uniform<size_t>(0, 5));
        }
        break;
    }
    case 4:
    {
        auto& v4 = meta.v4();
        v4.txChangesBefore = generateRandomLedgerEntryChanges();
        size_t numOps = rand_uniform<size_t>(1, 3);
        v4.operations.resize(numOps);
        for (auto& opMeta : v4.operations)
        {
            opMeta.events.resize(rand_uniform<size_t>(0, 5));
            opMeta.changes = generateRandomLedgerEntryChanges();
        }
        v4.txChangesAfter = generateRandomLedgerEntryChanges();
        if (rand_flip())
        {
            v4.sorobanMeta.activate();
            v4.sorobanMeta->ext.v(1);
            v4.sorobanMeta->returnValue.activate();
            v4.sorobanMeta->returnValue->type(SCV_BYTES);
            v4.sorobanMeta->returnValue->bytes().resize(
                rand_uniform<size_t>(0, 10));
        }
        break;
    }
    }

    return meta;
}

// Helper function to generate LedgerCloseMeta of given version with
// specified number of transactions
LedgerCloseMeta
generateLedgerCloseMeta(int version, size_t numTxs, std::vector<Hash>& txHashes)
{
    LedgerCloseMeta lcm;
    lcm.v(version);
    txHashes.clear();
    txHashes.reserve(numTxs);

    // Generate random hashes for transactions
    for (size_t i = 0; i < numTxs; ++i)
    {
        txHashes.push_back(generateRandomHash());
    }
    switch (version)
    {
    case 0:
    {
        auto& v0 = lcm.v0();

        // Populate transaction set
        v0.txSet.previousLedgerHash = generateRandomHash();
        v0.txSet.txs.resize(numTxs);

        // Populate transaction processing
        v0.txProcessing.resize(numTxs);
        for (size_t i = 0; i < numTxs; ++i)
        {
            auto& txMeta = v0.txProcessing[i];
            txMeta.result.transactionHash = txHashes[i];
            txMeta.result.result = generateRandomTransactionResult();
            txMeta.feeProcessing = generateRandomLedgerEntryChanges();
            txMeta.txApplyProcessing = generateRandomTransactionMeta();
        }

        v0.scpInfo.resize(3);
        break;
    }
    case 1:
    {
        auto& v1 = lcm.v1();

        // Populate GeneralizedTransactionSet
        v1.txSet.v(1);
        v1.txSet.v1TxSet().previousLedgerHash = generateRandomHash();
        v1.txSet.v1TxSet().phases.resize(2);
        v1.txSet.v1TxSet().phases[0].v0Components().resize(numTxs / 2);
        v1.txSet.v1TxSet().phases[1].v0Components().resize(numTxs / 2);

        // Populate transaction processing (same as v0)
        v1.txProcessing.resize(numTxs);
        for (size_t i = 0; i < numTxs; ++i)
        {
            auto& txMeta = v1.txProcessing[i];
            txMeta.result.transactionHash = txHashes[i];
            txMeta.result.result = generateRandomTransactionResult();
            txMeta.feeProcessing = generateRandomLedgerEntryChanges();
            txMeta.txApplyProcessing = generateRandomTransactionMeta();
        }

        v1.scpInfo.resize(3);
        auto keys =
            LedgerTestUtils::generateUniqueValidSorobanLedgerEntryKeys(5);
        v1.evictedKeys.assign(keys.begin(), keys.end());
        break;
    }
    case 2:
    {
        auto& v2 = lcm.v2();
        for (int i = 0; i < 4; ++i)
        {
            v2.ledgerHeader.header.skipList[i] = generateRandomHash();
        }

        // Populate GeneralizedTransactionSet (same as v1)
        v2.txSet.v(1);
        v2.txSet.v1TxSet().previousLedgerHash = generateRandomHash();
        v2.txSet.v1TxSet().phases.resize(2);
        v2.txSet.v1TxSet().phases[0].v0Components().resize(numTxs / 2);
        v2.txSet.v1TxSet().phases[1].v0Components().resize(numTxs / 2);

        // Populate transaction processing with TransactionResultMetaV1
        v2.txProcessing.resize(numTxs);
        for (size_t i = 0; i < numTxs; ++i)
        {
            auto& txMeta = v2.txProcessing[i];
            txMeta.ext.v(0);
            txMeta.result.transactionHash = txHashes[i];
            txMeta.result.result = generateRandomTransactionResult();
            txMeta.feeProcessing = generateRandomLedgerEntryChanges();
            txMeta.txApplyProcessing = generateRandomTransactionMeta();
            txMeta.postTxApplyFeeProcessing =
                generateRandomLedgerEntryChanges();
        }

        v2.scpInfo.resize(3);
        auto keys =
            LedgerTestUtils::generateUniqueValidSorobanLedgerEntryKeys(5);
        v2.evictedKeys.assign(keys.begin(), keys.end());
        break;
    }
    }
    return lcm;
}

TEST_CASE("LedgerCloseMetaOffsetFinder", "[lcm][offset]")
{
    SECTION("finds offsets for all LedgerCloseMeta versions")
    {
        for (int version = 0; version <= 2; ++version)
        {
            SECTION(fmt::format("version {}", version))
            {
                // Generate test data
                size_t numTxs = rand_uniform<size_t>(10, 20);
                std::vector<Hash> txHashes;
                auto lcm = generateLedgerCloseMeta(version, numTxs, txHashes);

                // Encode to XDR
                auto xdrData = xdr::xdr_to_opaque(lcm);

                for (size_t targetIdx = 0; targetIdx < numTxs; ++targetIdx)
                {
                    Hash targetHash = txHashes[targetIdx];

                    // LedgerCloseMeta lcm;
                    //// xdr_argpack_archive(finder, lcm);
                    // xdr_get arch(xdrData.data(),
                    //             xdrData.data() + xdrData.size());
                    // xdr_argpack_archive(finder, lcm);

                    // Find offsets
                    auto maybeOffsets =
                        findTransactionResultMetaOffsets(xdrData, targetHash);

                    REQUIRE(maybeOffsets.has_value());
                    auto offsets = maybeOffsets.value();

                    // Extract the found transaction result meta using
                    // offsets
                    REQUIRE(offsets.startOffset < offsets.endOffset);
                    REQUIRE(offsets.endOffset <= xdrData.size());

                    size_t metaSize = offsets.endOffset - offsets.startOffset;
                    xdr::xdr_get getter(xdrData.data() + offsets.startOffset,
                                        xdrData.data() + offsets.endOffset);

                    if (version == 2)
                    {
                        // For version 2, we should get
                        // TransactionResultMetaV1
                        TransactionResultMetaV1 extractedMeta;
                        xdr::xdr_argpack_archive(getter, extractedMeta);

                        // Compare with original
                        auto& originalMeta =
                            lcm.v2().txProcessing.at(targetIdx);
                        REQUIRE(originalMeta == extractedMeta);
                    }
                    else
                    {
                        // For versions 0 and 1, we should get
                        // TransactionResultMeta
                        TransactionResultMeta extractedMeta;
                        xdr::xdr_argpack_archive(getter, extractedMeta);

                        // Get original meta based on version
                        TransactionResultMeta const* originalMeta = nullptr;
                        if (version == 0)
                        {
                            originalMeta = &lcm.v0().txProcessing.at(targetIdx);
                        }
                        else if (version == 1)
                        {
                            originalMeta = &lcm.v1().txProcessing.at(targetIdx);
                        }

                        REQUIRE(originalMeta != nullptr);
                        REQUIRE(*originalMeta == extractedMeta);
                    }
                }
            }
        }
    }

    SECTION("returns nullopt for non-existent transaction hash")
    {
        std::vector<Hash> txHashes;
        auto lcm = generateLedgerCloseMeta(1, 3, txHashes);
        auto xdrData = xdr::xdr_to_opaque(lcm);

        // Generate a hash that doesn't exist in the transaction set
        Hash nonExistentHash = generateRandomHash();

        auto maybeOffsets =
            findTransactionResultMetaOffsets(xdrData, nonExistentHash);
        REQUIRE_FALSE(maybeOffsets.has_value());
    }

    SECTION("handles empty transaction processing arrays")
    {
        std::vector<Hash> txHashes;
        auto lcm = generateLedgerCloseMeta(0, 0, txHashes);
        auto xdrData = xdr::xdr_to_opaque(lcm);

        Hash anyHash = generateRandomHash();
        auto maybeOffsets = findTransactionResultMetaOffsets(xdrData, anyHash);
        REQUIRE_FALSE(maybeOffsets.has_value());
    }

    // SECTION("handles malformed XDR data")
    //{
    //    // Create truncated XDR data
    //    xdr::xvector<uint8_t> malformedData = {0x00, 0x00, 0x00,
    //                                           0x01}; // Just a version field

    //    Hash anyHash = generateRandomHash();
    //    auto maybeOffsets =
    //        findTransactionResultMetaOffsets(malformedData, anyHash);
    //    REQUIRE_FALSE(maybeOffsets.has_value());
    //}
}

struct BenchmarkQuery
{
    int version;
    xdr::xvector<uint8_t> xdrData;
    Hash targetHash;
    bool shouldExist;
};

// Linear search implementation for comparison
std::optional<std::variant<TransactionResultMeta, TransactionResultMetaV1>>
findTransactionResultMetaLinear(LedgerCloseMeta const& lcm,
                                Hash const& targetHash)
{
    switch (lcm.v())
    {
    case 0:
    {
        for (auto const& txMeta : lcm.v0().txProcessing)
        {
            if (txMeta.result.transactionHash == targetHash)
            {
                return std::make_optional(txMeta);
            }
        }
        break;
    }
    case 1:
    {
        for (auto const& txMeta : lcm.v1().txProcessing)
        {
            if (txMeta.result.transactionHash == targetHash)
            {
                return std::make_optional(txMeta);
            }
        }
        break;
    }
    case 2:
    {
        // For v2, we need to convert TransactionResultMetaV1 to
        // TransactionResultMeta This is a simplification for benchmark purposes
        for (auto const& txMetaV1 : lcm.v2().txProcessing)
        {
            if (txMetaV1.result.transactionHash == targetHash)
            {
                return std::make_optional(txMetaV1);
            }
        }
        break;
    }
    default:
        releaseAssert(false);
    }
    return std::nullopt;
}

// Generate benchmark queries
std::vector<BenchmarkQuery>
generateBenchmarkQueries(size_t numTxs, size_t numQueries)
{
    std::vector<BenchmarkQuery> queries;
    queries.reserve(numQueries);

    for (size_t i = 0; i < numQueries; ++i)
    {
        BenchmarkQuery query;
        std::vector<Hash> txHashes;
        query.version = rand_uniform<int>(0, 2);
        auto lcm = generateLedgerCloseMeta(query.version, numTxs, txHashes);

        // Encode to XDR once for offset finder
        query.xdrData = xdr::xdr_to_opaque(lcm);

        // 10% probability of non-existent hash
        if (rand_uniform<uint32_t>(1, 100) <= 10)
        {
            query.targetHash = generateRandomHash();
            query.shouldExist = false;
        }
        else
        {
            // Pick a random hash from valid ones
            size_t idx = rand_uniform<size_t>(0, txHashes.size() - 1);
            query.targetHash = txHashes[idx];
            query.shouldExist = true;
        }

        queries.push_back(query);
    }

    return queries;
}

TEST_CASE("LedgerCloseMetaOffsetFinder Benchmark", "[lcm][offset][benchmark]")
{
    constexpr size_t NUM_TRANSACTIONS = 1000;
    constexpr size_t NUM_QUERIES = 1000;

    SECTION("benchmark against linear search")
    {

        auto queries = generateBenchmarkQueries(NUM_TRANSACTIONS, NUM_QUERIES);

        // Benchmark offset finder
        auto startOffset = std::chrono::high_resolution_clock::now();

        std::vector<std::optional<
            std::variant<TransactionResultMeta, TransactionResultMetaV1>>>
            extractedMetas;
        extractedMetas.reserve(NUM_QUERIES);
        for (auto const& query : queries)
        {
            auto maybeOffsets = findTransactionResultMetaOffsets(
                query.xdrData, query.targetHash);
            if (maybeOffsets.has_value())
            {
                REQUIRE(query.shouldExist);
                xdr::xdr_get getter(
                    query.xdrData.data() + maybeOffsets->startOffset,
                    query.xdrData.data() + maybeOffsets->endOffset);
                if (query.version < 2)
                {
                    TransactionResultMeta extractedMeta;
                    xdr::xdr_argpack_archive(getter, extractedMeta);
                    extractedMetas.emplace_back(extractedMeta);
                }
                else
                {
                    TransactionResultMetaV1 extractedMeta;
                    xdr::xdr_argpack_archive(getter, extractedMeta);
                    extractedMetas.emplace_back(extractedMeta);
                }
            }
            else
            {
                REQUIRE(!query.shouldExist);
                extractedMetas.push_back(std::nullopt);
            }
        }

        auto endOffset = std::chrono::high_resolution_clock::now();
        auto offsetDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(endOffset -
                                                                  startOffset);

        std::vector<std::optional<
            std::variant<TransactionResultMeta, TransactionResultMetaV1>>>
            extractedMetasLinear;
        extractedMetasLinear.reserve(NUM_QUERIES);
        // Also benchmark full deserialization + linear search
        auto startFullLinear = std::chrono::high_resolution_clock::now();

        size_t fullLinearFoundCount = 0;
        for (auto const& query : queries)
        {
            LedgerCloseMeta tempLcm;
            xdr_from_opaque(query.xdrData, tempLcm);
            extractedMetasLinear.emplace_back(
                findTransactionResultMetaLinear(tempLcm, query.targetHash));
        }

        auto endFullLinear = std::chrono::high_resolution_clock::now();
        auto fullLinearDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                endFullLinear - startFullLinear);

        REQUIRE(extractedMetas == extractedMetasLinear);

        // Print benchmark results
        double offsetTimeMs = offsetDuration.count() / 1000.0;
        double fullLinearTimeMs = fullLinearDuration.count() / 1000.0;
        double speedupVsFullLinear = fullLinearTimeMs / offsetTimeMs;

        std::cout << "LCM Benchmark Results:" << std::endl;
        std::cout << fmt::format("  Transactions: {}", NUM_TRANSACTIONS)
                  << std::endl;
        std::cout << fmt::format("  Queries: {}", NUM_QUERIES) << std::endl;
        std::cout << fmt::format("  Offset Finder: {:.2f} ms ({:.5f} ms/query)",
                                 offsetTimeMs, offsetTimeMs / NUM_QUERIES)
                  << std::endl;
        std::cout
            << fmt::format(
                   "  Linear Search (with deserialization): {:.2f} ms ({:.5f} "
                   "ms/query)",
                   fullLinearTimeMs, fullLinearTimeMs / NUM_QUERIES)
            << std::endl;
        std::cout << fmt::format("  Speedup vs full linear: {:.2f}x",
                                 speedupVsFullLinear)
                  << std::endl;
    }
}

} // namespace

} // namespace stellar
