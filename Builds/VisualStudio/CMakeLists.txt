cmake_minimum_required(VERSION 3.16.0 FATAL_ERROR)

set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "" FORCE)

project(stellar-core C CXX)

################################################################################
# Set target arch type if empty. Visual studio solution generator provides it.
################################################################################
if(NOT CMAKE_VS_PLATFORM_NAME)
    set(CMAKE_VS_PLATFORM_NAME "x64")
endif()
message("${CMAKE_VS_PLATFORM_NAME} architecture in use")

if(NOT ("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64"))
    message(FATAL_ERROR "${CMAKE_VS_PLATFORM_NAME} arch is not supported!")
endif()

################################################################################
# Global configuration types
################################################################################
set(CMAKE_CONFIGURATION_TYPES
    "Debug"
    "DebugNoPostgres"
    "Release"
    CACHE STRING "" FORCE
)

################################################################################
# Global compiler options
################################################################################
if(MSVC)
    # remove default flags provided with CMake for MSVC
    set(CMAKE_C_FLAGS "")
    set(CMAKE_C_FLAGS_DEBUG "")
    set(CMAKE_C_FLAGS_DEBUGNOPOSTGRES "")
    set(CMAKE_C_FLAGS_RELEASE "")
    set(CMAKE_CXX_FLAGS "")
    set(CMAKE_CXX_FLAGS_DEBUG "")
    set(CMAKE_CXX_FLAGS_DEBUGNOPOSTGRES "")
    set(CMAKE_CXX_FLAGS_RELEASE "")
endif()

################################################################################
# Global linker options
################################################################################
if(MSVC)
    # remove default flags provided with CMake for MSVC
    set(CMAKE_EXE_LINKER_FLAGS "")
    set(CMAKE_MODULE_LINKER_FLAGS "")
    set(CMAKE_SHARED_LINKER_FLAGS "")
    set(CMAKE_STATIC_LINKER_FLAGS "")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS}")
    set(CMAKE_MODULE_LINKER_FLAGS_DEBUG "${CMAKE_MODULE_LINKER_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS}")
    set(CMAKE_STATIC_LINKER_FLAGS_DEBUG "${CMAKE_STATIC_LINKER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUGNOPOSTGRES "${CMAKE_EXE_LINKER_FLAGS}")
    set(CMAKE_MODULE_LINKER_FLAGS_DEBUGNOPOSTGRES "${CMAKE_MODULE_LINKER_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS_DEBUGNOPOSTGRES "${CMAKE_SHARED_LINKER_FLAGS}")
    set(CMAKE_STATIC_LINKER_FLAGS_DEBUGNOPOSTGRES "${CMAKE_STATIC_LINKER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS}")
    set(CMAKE_MODULE_LINKER_FLAGS_RELEASE "${CMAKE_MODULE_LINKER_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS}")
    set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS}")
endif()

################################################################################
# Nuget packages function stub.
################################################################################
function(use_package TARGET PACKAGE VERSION)
    message(WARNING "No implementation of use_package. Create yours. "
                    "Package \"${PACKAGE}\" with version \"${VERSION}\" "
                    "for target \"${TARGET}\" is ignored!")
endfunction()

################################################################################
# Common utils
################################################################################
include(CMake/Utils.cmake)

################################################################################
# Additional Global Settings(add specific info there)
################################################################################
include(CMake/GlobalSettingsInclude.cmake OPTIONAL)

################################################################################
# Use solution folders feature
################################################################################
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

################################################################################
# Sub-projects
################################################################################
add_subdirectory(../../lib/libsodium/builds/msvc/vs2019/libsodium ${CMAKE_BINARY_DIR}/libsodium)
add_subdirectory(../../lib/xdrpp/msvc_xdrpp/xdrc ${CMAKE_BINARY_DIR}/xdrc)
add_subdirectory(../../lib/xdrpp/msvc_xdrpp/xdrpp ${CMAKE_BINARY_DIR}/xdrpp)
add_subdirectory(libmedida)
add_subdirectory(libsoci)
add_subdirectory(libsoci_postgresql)
add_subdirectory(libsoci_sqlite3)
add_subdirectory(libsqlite)



























set(PROJECT_NAME stellar-core)

################################################################################
# Source groups
################################################################################
set(no_group_source_files
    "../../AUTHORS"
    "../../autogen.sh"
    "../../ChangeLog"
    "../../ci-build.sh"
    "../../CONTRIBUTING.md"
    "../../COPYING"
    "../../INSTALL-Windows.md"
    "../../INSTALL.md"
    "../../LICENSE-APACHE.txt"
    "../../Makefile.am"
    "../../NEWS"
    "../../performance-eval/performance-eval.md"
    "../../README"
    "../../README.md"
    "libmedida/libmedida.vcxproj"
)
source_group("" FILES ${no_group_source_files})

set(bucket
    "../../src/bucket/Bucket.cpp"
    "../../src/bucket/Bucket.h"
    "../../src/bucket/BucketApplicator.cpp"
    "../../src/bucket/BucketApplicator.h"
    "../../src/bucket/BucketInputIterator.cpp"
    "../../src/bucket/BucketInputIterator.h"
    "../../src/bucket/BucketList.cpp"
    "../../src/bucket/BucketList.h"
    "../../src/bucket/BucketManager.h"
    "../../src/bucket/BucketManagerImpl.cpp"
    "../../src/bucket/BucketManagerImpl.h"
    "../../src/bucket/BucketMergeMap.cpp"
    "../../src/bucket/BucketMergeMap.h"
    "../../src/bucket/BucketOutputIterator.cpp"
    "../../src/bucket/BucketOutputIterator.h"
    "../../src/bucket/BucketTests.h"
    "../../src/bucket/FutureBucket.cpp"
    "../../src/bucket/FutureBucket.h"
    "../../src/bucket/LedgerCmp.h"
    "../../src/bucket/MergeKey.cpp"
    "../../src/bucket/MergeKey.h"
    "../../src/bucket/PublishQueueBuckets.cpp"
    "../../src/bucket/PublishQueueBuckets.h"
    "../../src/bucket/readme.md"
)
source_group("bucket" FILES ${bucket})

set(bucket__tests
    "../../src/bucket/test/BucketListTests.cpp"
    "../../src/bucket/test/BucketManagerTests.cpp"
    "../../src/bucket/test/BucketMergeMapTests.cpp"
    "../../src/bucket/test/BucketTests.cpp"
)
source_group("bucket\\tests" FILES ${bucket__tests})

set(catchup
    "../../src/catchup/ApplyBucketsWork.cpp"
    "../../src/catchup/ApplyBucketsWork.h"
    "../../src/catchup/ApplyBufferedLedgersWork.cpp"
    "../../src/catchup/ApplyBufferedLedgersWork.h"
    "../../src/catchup/ApplyCheckpointWork.cpp"
    "../../src/catchup/ApplyCheckpointWork.h"
    "../../src/catchup/ApplyLedgerWork.cpp"
    "../../src/catchup/ApplyLedgerWork.h"
    "../../src/catchup/CatchupConfiguration.cpp"
    "../../src/catchup/CatchupConfiguration.h"
    "../../src/catchup/CatchupManager.h"
    "../../src/catchup/CatchupManagerImpl.cpp"
    "../../src/catchup/CatchupManagerImpl.h"
    "../../src/catchup/CatchupRange.cpp"
    "../../src/catchup/CatchupRange.h"
    "../../src/catchup/CatchupWork.cpp"
    "../../src/catchup/CatchupWork.h"
    "../../src/catchup/DownloadApplyTxsWork.cpp"
    "../../src/catchup/DownloadApplyTxsWork.h"
    "../../src/catchup/VerifyLedgerChainWork.cpp"
    "../../src/catchup/VerifyLedgerChainWork.h"
)
source_group("catchup" FILES ${catchup})

set(catchup__simulation
    "../../src/catchup/simulation/HistoryArchiveStream.cpp"
    "../../src/catchup/simulation/HistoryArchiveStream.h"
    "../../src/catchup/simulation/TxSimApplyTransactionsWork.cpp"
    "../../src/catchup/simulation/TxSimApplyTransactionsWork.h"
)
source_group("catchup\\simulation" FILES ${catchup__simulation})

set(catchup__tests
    "../../src/catchup/test/CatchupWorkTests.cpp"
    "../../src/catchup/test/CatchupWorkTests.h"
)
source_group("catchup\\tests" FILES ${catchup__tests})

set(crypto
    "../../src/crypto/BLAKE2.cpp"
    "../../src/crypto/BLAKE2.h"
    "../../src/crypto/ByteSlice.h"
    "../../src/crypto/Curve25519.cpp"
    "../../src/crypto/Curve25519.h"
    "../../src/crypto/Hex.cpp"
    "../../src/crypto/Hex.h"
    "../../src/crypto/KeyUtils.cpp"
    "../../src/crypto/KeyUtils.h"
    "../../src/crypto/Random.cpp"
    "../../src/crypto/Random.h"
    "../../src/crypto/readme.md"
    "../../src/crypto/SecretKey.cpp"
    "../../src/crypto/SecretKey.h"
    "../../src/crypto/SHA.cpp"
    "../../src/crypto/SHA.h"
    "../../src/crypto/ShortHash.cpp"
    "../../src/crypto/ShortHash.h"
    "../../src/crypto/SignerKey.cpp"
    "../../src/crypto/SignerKey.h"
    "../../src/crypto/SignerKeyUtils.cpp"
    "../../src/crypto/SignerKeyUtils.h"
    "../../src/crypto/StrKey.cpp"
    "../../src/crypto/StrKey.h"
    "../../src/crypto/XDRHasher.h"
)
source_group("crypto" FILES ${crypto})

set(crypto__tests
    "../../src/crypto/test/CryptoTests.cpp"
    "../../src/crypto/test/ShortHashTests.cpp"
)
source_group("crypto\\tests" FILES ${crypto__tests})

set(database
    "../../src/database/Database.cpp"
    "../../src/database/Database.h"
    "../../src/database/DatabaseConnectionString.cpp"
    "../../src/database/DatabaseConnectionString.h"
    "../../src/database/DatabaseTypeSpecificOperation.h"
    "../../src/database/DatabaseUtils.cpp"
    "../../src/database/DatabaseUtils.h"
    "../../src/database/readme.md"
)
source_group("database" FILES ${database})

set(database__tests
    "../../src/database/test/DatabaseConnectionStringTest.cpp"
    "../../src/database/test/DatabaseTests.cpp"
)
source_group("database\\tests" FILES ${database__tests})

set(docs
    "../../docs/architecture.md"
    "../../docs/db-schema.md"
    "../../docs/fuzzing.md"
    "../../docs/history.md"
    "../../docs/integration.md"
    "../../docs/ledger.md"
    "../../docs/metrics.md"
    "../../docs/quick-reference.md"
    "../../docs/readme.md"
    "../../docs/stellar-core_example.cfg"
    "../../docs/stellar-core_example_validators.cfg"
    "../../docs/stellar-core_standalone.cfg"
    "../../docs/stellar-core_testnet.cfg"
    "../../docs/stellar-core_testnet_legacy.cfg"
    "../../docs/stellar-core_testnet_validator.cfg"
    "../../docs/transaction.md"
    "../../docs/tx-simulation.md"
    "../../docs/versioning.md"
)
source_group("docs" FILES ${docs})

set(docs__software
    "../../docs/software/admin.md"
    "../../docs/software/commands.md"
    "../../docs/software/core-data-flow.pdf"
    "../../docs/software/performance.md"
    "../../docs/software/security-protocol-release-notes.md"
    "../../docs/software/testnet.md"
)
source_group("docs\\software" FILES ${docs__software})

set(github
    "../../.github/ISSUE_TEMPLATE/bug-report.md"
    "../../.github/ISSUE_TEMPLATE/documentation.md"
    "../../.github/ISSUE_TEMPLATE/feature-request.md"
    "../../.github/PULL_REQUEST_TEMPLATE.md"
)
source_group("github" FILES ${github})

set(herder
    "../../src/herder/Herder.cpp"
    "../../src/herder/Herder.h"
    "../../src/herder/HerderImpl.cpp"
    "../../src/herder/HerderImpl.h"
    "../../src/herder/HerderPersistence.h"
    "../../src/herder/HerderPersistenceImpl.cpp"
    "../../src/herder/HerderPersistenceImpl.h"
    "../../src/herder/HerderSCPDriver.cpp"
    "../../src/herder/HerderSCPDriver.h"
    "../../src/herder/HerderUtils.cpp"
    "../../src/herder/HerderUtils.h"
    "../../src/herder/LedgerCloseData.cpp"
    "../../src/herder/LedgerCloseData.h"
    "../../src/herder/PendingEnvelopes.cpp"
    "../../src/herder/PendingEnvelopes.h"
    "../../src/herder/QuorumIntersectionChecker.h"
    "../../src/herder/QuorumIntersectionCheckerImpl.cpp"
    "../../src/herder/QuorumIntersectionCheckerImpl.h"
    "../../src/herder/QuorumTracker.cpp"
    "../../src/herder/QuorumTracker.h"
    "../../src/herder/SurgePricingUtils.cpp"
    "../../src/herder/SurgePricingUtils.h"
    "../../src/herder/TransactionQueue.cpp"
    "../../src/herder/TransactionQueue.h"
    "../../src/herder/TxQueueLimiter.cpp"
    "../../src/herder/TxQueueLimiter.h"
    "../../src/herder/TxSetFrame.cpp"
    "../../src/herder/TxSetFrame.h"
    "../../src/herder/Upgrades.cpp"
    "../../src/herder/Upgrades.h"
)
source_group("herder" FILES ${herder})

set(herder__docs
    "../../src/herder/readme.md"
)
source_group("herder\\docs" FILES ${herder__docs})

set(herder__simulation
    "../../src/herder/simulation/TxSimTxSetFrame.cpp"
    "../../src/herder/simulation/TxSimTxSetFrame.h"
)
source_group("herder\\simulation" FILES ${herder__simulation})

set(herder__tests
    "../../src/herder/test/HerderTests.cpp"
    "../../src/herder/test/PendingEnvelopesTests.cpp"
    "../../src/herder/test/QuorumIntersectionTests.cpp"
    "../../src/herder/test/QuorumTrackerTests.cpp"
    "../../src/herder/test/TransactionQueueTests.cpp"
    "../../src/herder/test/UpgradesTests.cpp"
)
source_group("herder\\tests" FILES ${herder__tests})

set(history
    "../../src/history/FileTransferInfo.cpp"
    "../../src/history/FileTransferInfo.h"
    "../../src/history/HistoryArchive.cpp"
    "../../src/history/HistoryArchive.h"
    "../../src/history/HistoryArchiveManager.cpp"
    "../../src/history/HistoryArchiveManager.h"
    "../../src/history/HistoryArchiveReportWork.cpp"
    "../../src/history/HistoryArchiveReportWork.h"
    "../../src/history/HistoryManager.h"
    "../../src/history/HistoryManagerImpl.cpp"
    "../../src/history/HistoryManagerImpl.h"
    "../../src/history/HistoryTestsUtils.h"
    "../../src/history/readme.md"
    "../../src/history/StateSnapshot.cpp"
    "../../src/history/StateSnapshot.h"
)
source_group("history" FILES ${history})

set(historyWork
    "../../src/historywork/BatchDownloadWork.cpp"
    "../../src/historywork/BatchDownloadWork.h"
    "../../src/historywork/CheckSingleLedgerHeaderWork.cpp"
    "../../src/historywork/CheckSingleLedgerHeaderWork.h"
    "../../src/historywork/DownloadBucketsWork.cpp"
    "../../src/historywork/DownloadBucketsWork.h"
    "../../src/historywork/DownloadVerifyTxResultsWork.cpp"
    "../../src/historywork/DownloadVerifyTxResultsWork.h"
    "../../src/historywork/FetchRecentQsetsWork.cpp"
    "../../src/historywork/FetchRecentQsetsWork.h"
    "../../src/historywork/GetAndUnzipRemoteFileWork.cpp"
    "../../src/historywork/GetAndUnzipRemoteFileWork.h"
    "../../src/historywork/GetHistoryArchiveStateWork.cpp"
    "../../src/historywork/GetHistoryArchiveStateWork.h"
    "../../src/historywork/GetRemoteFileWork.cpp"
    "../../src/historywork/GetRemoteFileWork.h"
    "../../src/historywork/GunzipFileWork.cpp"
    "../../src/historywork/GunzipFileWork.h"
    "../../src/historywork/GzipFileWork.cpp"
    "../../src/historywork/GzipFileWork.h"
    "../../src/historywork/MakeRemoteDirWork.cpp"
    "../../src/historywork/MakeRemoteDirWork.h"
    "../../src/historywork/Progress.cpp"
    "../../src/historywork/Progress.h"
    "../../src/historywork/PublishWork.cpp"
    "../../src/historywork/PublishWork.h"
    "../../src/historywork/PutFilesWork.cpp"
    "../../src/historywork/PutFilesWork.h"
    "../../src/historywork/PutHistoryArchiveStateWork.cpp"
    "../../src/historywork/PutHistoryArchiveStateWork.h"
    "../../src/historywork/PutRemoteFileWork.cpp"
    "../../src/historywork/PutRemoteFileWork.h"
    "../../src/historywork/PutSnapshotFilesWork.cpp"
    "../../src/historywork/PutSnapshotFilesWork.h"
    "../../src/historywork/ResolveSnapshotWork.cpp"
    "../../src/historywork/ResolveSnapshotWork.h"
    "../../src/historywork/RunCommandWork.cpp"
    "../../src/historywork/RunCommandWork.h"
    "../../src/historywork/VerifyBucketWork.cpp"
    "../../src/historywork/VerifyBucketWork.h"
    "../../src/historywork/VerifyTxResultsWork.cpp"
    "../../src/historywork/VerifyTxResultsWork.h"
    "../../src/historywork/WriteSnapshotWork.cpp"
    "../../src/historywork/WriteSnapshotWork.h"
    "../../src/historywork/WriteVerifiedCheckpointHashesWork.cpp"
    "../../src/historywork/WriteVerifiedCheckpointHashesWork.h"
)
source_group("historyWork" FILES ${historyWork})

set(historyWork__tests
    "../../src/historywork/test/HistoryWorkTests.cpp"
)
source_group("historyWork\\tests" FILES ${historyWork__tests})

set(history__tests
    "../../src/history/test/HistoryTests.cpp"
    "../../src/history/test/HistoryTestsUtils.cpp"
    "../../src/history/test/HistoryTestsUtils.h"
    "../../src/history/test/SerializeTests.cpp"
)
source_group("history\\tests" FILES ${history__tests})

set(history__tests__serialize-tests
    "../../src/history/serialize-tests/stellar-history.livenet.15686975.json"
    "../../src/history/serialize-tests/stellar-history.testnet.6714239.json"
)
source_group("history\\tests\\serialize-tests" FILES ${history__tests__serialize-tests})

set(invariant
    "../../src/invariant/AccountSubEntriesCountIsValid.cpp"
    "../../src/invariant/AccountSubEntriesCountIsValid.h"
    "../../src/invariant/BucketListIsConsistentWithDatabase.cpp"
    "../../src/invariant/BucketListIsConsistentWithDatabase.h"
    "../../src/invariant/ConservationOfLumens.cpp"
    "../../src/invariant/ConservationOfLumens.h"
    "../../src/invariant/ConstantProductInvariant.cpp"
    "../../src/invariant/ConstantProductInvariant.h"
    "../../src/invariant/Invariant.h"
    "../../src/invariant/InvariantDoesNotHold.h"
    "../../src/invariant/InvariantManager.h"
    "../../src/invariant/InvariantManagerImpl.cpp"
    "../../src/invariant/InvariantManagerImpl.h"
    "../../src/invariant/LedgerEntryIsValid.cpp"
    "../../src/invariant/LedgerEntryIsValid.h"
    "../../src/invariant/LiabilitiesMatchOffers.cpp"
    "../../src/invariant/LiabilitiesMatchOffers.h"
    "../../src/invariant/OrderBookIsNotCrossed.cpp"
    "../../src/invariant/OrderBookIsNotCrossed.h"
    "../../src/invariant/SponsorshipCountIsValid.cpp"
    "../../src/invariant/SponsorshipCountIsValid.h"
)
source_group("invariant" FILES ${invariant})

set(invariant__tests
    "../../src/invariant/test/AccountSubEntriesCountIsValidTests.cpp"
    "../../src/invariant/test/BucketListIsConsistentWithDatabaseTests.cpp"
    "../../src/invariant/test/ConservationOfLumensTests.cpp"
    "../../src/invariant/test/InvariantTests.cpp"
    "../../src/invariant/test/InvariantTestUtils.cpp"
    "../../src/invariant/test/InvariantTestUtils.h"
    "../../src/invariant/test/LiabilitiesMatchOffersTests.cpp"
    "../../src/invariant/test/OrderBookIsNotCrossedTests.cpp"
    "../../src/invariant/test/SponsorshipCountIsValidTests.cpp"
)
source_group("invariant\\tests" FILES ${invariant__tests})

set(ledger
    "../../src/ledger/CheckpointRange.cpp"
    "../../src/ledger/CheckpointRange.h"
    "../../src/ledger/FlushAndRotateMetaDebugWork.cpp"
    "../../src/ledger/FlushAndRotateMetaDebugWork.h"
    "../../src/ledger/InMemoryLedgerTxn.cpp"
    "../../src/ledger/InMemoryLedgerTxn.h"
    "../../src/ledger/InMemoryLedgerTxnRoot.cpp"
    "../../src/ledger/InMemoryLedgerTxnRoot.h"
    "../../src/ledger/InternalLedgerEntry.cpp"
    "../../src/ledger/InternalLedgerEntry.h"
    "../../src/ledger/LedgerHashUtils.h"
    "../../src/ledger/LedgerHeaderUtils.cpp"
    "../../src/ledger/LedgerHeaderUtils.h"
    "../../src/ledger/LedgerManager.h"
    "../../src/ledger/LedgerManagerImpl.cpp"
    "../../src/ledger/LedgerManagerImpl.h"
    "../../src/ledger/LedgerRange.cpp"
    "../../src/ledger/LedgerRange.h"
    "../../src/ledger/LedgerTxn.cpp"
    "../../src/ledger/LedgerTxn.h"
    "../../src/ledger/LedgerTxnAccountSQL.cpp"
    "../../src/ledger/LedgerTxnClaimableBalanceSQL.cpp"
    "../../src/ledger/LedgerTxnDataSQL.cpp"
    "../../src/ledger/LedgerTxnEntry.cpp"
    "../../src/ledger/LedgerTxnEntry.h"
    "../../src/ledger/LedgerTxnHeader.cpp"
    "../../src/ledger/LedgerTxnHeader.h"
    "../../src/ledger/LedgerTxnImpl.h"
    "../../src/ledger/LedgerTxnLiquidityPoolSQL.cpp"
    "../../src/ledger/LedgerTxnOfferSQL.cpp"
    "../../src/ledger/LedgerTxnTrustLineSQL.cpp"
    "../../src/ledger/NonSociRelatedException.h"
    "../../src/ledger/readme.md"
    "../../src/ledger/TrustLineWrapper.cpp"
    "../../src/ledger/TrustLineWrapper.h"
)
source_group("ledger" FILES ${ledger})

set(ledger__tests
    "../../src/ledger/test/LedgerCloseMetaStreamTests.cpp"
    "../../src/ledger/test/LedgerHeaderTests.cpp"
    "../../src/ledger/test/LedgerTests.cpp"
    "../../src/ledger/test/LedgerTestUtils.cpp"
    "../../src/ledger/test/LedgerTestUtils.h"
    "../../src/ledger/test/LedgerTxnTests.cpp"
    "../../src/ledger/test/LiabilitiesTests.cpp"
)
source_group("ledger\\tests" FILES ${ledger__tests})

set(lib
    "../../lib/asio.cpp"
    "../../lib/catch.hpp"
    "../../lib/spdlog.cpp"
)
source_group("lib" FILES ${lib})

set(lib__fmt
    "../../lib/fmt/include/fmt/format.h"
)
source_group("lib\\fmt" FILES ${lib__fmt})

set(lib__http
    "../../lib/http/connection.cpp"
    "../../lib/http/connection.hpp"
    "../../lib/http/connection_manager.cpp"
    "../../lib/http/connection_manager.hpp"
    "../../lib/http/header.hpp"
    "../../lib/http/HttpClient.cpp"
    "../../lib/http/HttpClient.h"
    "../../lib/http/reply.cpp"
    "../../lib/http/reply.hpp"
    "../../lib/http/request.hpp"
    "../../lib/http/request_parser.cpp"
    "../../lib/http/request_parser.hpp"
    "../../lib/http/server.cpp"
    "../../lib/http/server.hpp"
)
source_group("lib\\http" FILES ${lib__http})

set(lib__json
    "../../lib/json/json-forwards.h"
    "../../lib/json/json.h"
    "../../lib/json/jsoncpp.cpp"
)
source_group("lib\\json" FILES ${lib__json})

set(lib__spdlog
    "../../lib/spdlog/include/spdlog/sinks/basic_file_sink.h"
    "../../lib/spdlog/include/spdlog/sinks/stdout_color_sinks.h"
    "../../lib/spdlog/include/spdlog/sinks/stdout_sinks.h"
    "../../lib/spdlog/include/spdlog/spdlog.h"
)
source_group("lib\\spdlog" FILES ${lib__spdlog})

set(lib__tracy
    "../../lib/tracy/TracyClient.cpp"
)
source_group("lib\\tracy" FILES ${lib__tracy})

set(lib__util
    "../../lib/util/cpptoml.h"
    "../../lib/util/finally.h"
    "../../lib/util/getopt.h"
    "../../lib/util/getopt_long.c"
    "../../lib/util/siphash.cpp"
    "../../lib/util/siphash.h"
    "../../lib/util/uint128_t.h"
)
source_group("lib\\util" FILES ${lib__util})

set(lib__xdrpp
#    "../../lib/xdrpp/tests/arpc.cc"
#    "../../lib/xdrpp/tests/autocheck.cc"
#    "../../lib/xdrpp/tests/cereal.cc"
#    "../../lib/xdrpp/tests/compare.cc"
#    "../../lib/xdrpp/tests/listener.cc"
#    "../../lib/xdrpp/tests/marshal.cc"
#    "../../lib/xdrpp/tests/msgsock.cc"
#    "../../lib/xdrpp/tests/printer.cc"
#    "../../lib/xdrpp/tests/srpc.cc"
    "../../lib/xdrpp/tests/xdrtest.x"
)
source_group("lib\\xdrpp" FILES ${lib__xdrpp})

set(main
    "../../src/main/Application.cpp"
    "../../src/main/Application.h"
    "../../src/main/ApplicationImpl.cpp"
    "../../src/main/ApplicationImpl.h"
    "../../src/main/ApplicationUtils.cpp"
    "../../src/main/ApplicationUtils.h"
    "../../src/main/CommandHandler.cpp"
    "../../src/main/CommandHandler.h"
    "../../src/main/CommandLine.cpp"
    "../../src/main/CommandLine.h"
    "../../src/main/Config.cpp"
    "../../src/main/Config.h"
    "../../src/main/Diagnostics.cpp"
    "../../src/main/Diagnostics.h"
    "../../src/main/dumpxdr.cpp"
    "../../src/main/dumpxdr.h"
    "../../src/main/ErrorMessages.h"
    "../../src/main/ExternalQueue.cpp"
    "../../src/main/ExternalQueue.h"
    "../../src/main/main.cpp"
    "../../src/main/Maintainer.cpp"
    "../../src/main/Maintainer.h"
    "../../src/main/PersistentState.cpp"
    "../../src/main/PersistentState.h"
    "../../src/main/StellarCoreVersion.cpp.in"
    "../../src/main/StellarCoreVersion.h"
)
source_group("main" FILES ${main})

set(main__generated
    "../../src/main/StellarCoreVersion.cpp"
)
source_group("main\\generated" FILES ${main__generated})

set(main__tests
    "../../src/main/test/ApplicationUtilsTests.cpp"
    "../../src/main/test/CommandHandlerTests.cpp"
    "../../src/main/test/ConfigTests.cpp"
    "../../src/main/test/ExternalQueueTests.cpp"
    "../../src/main/test/SelfCheckTests.cpp"
)
source_group("main\\tests" FILES ${main__tests})

set(overlay
    "../../src/overlay/BanManager.h"
    "../../src/overlay/BanManagerImpl.cpp"
    "../../src/overlay/BanManagerImpl.h"
    "../../src/overlay/Floodgate.cpp"
    "../../src/overlay/Floodgate.h"
    "../../src/overlay/ItemFetcher.cpp"
    "../../src/overlay/ItemFetcher.h"
    "../../src/overlay/OverlayManager.h"
    "../../src/overlay/OverlayManagerImpl.cpp"
    "../../src/overlay/OverlayManagerImpl.h"
    "../../src/overlay/OverlayMetrics.cpp"
    "../../src/overlay/OverlayMetrics.h"
    "../../src/overlay/Peer.cpp"
    "../../src/overlay/Peer.h"
    "../../src/overlay/PeerAuth.cpp"
    "../../src/overlay/PeerAuth.h"
    "../../src/overlay/PeerBareAddress.cpp"
    "../../src/overlay/PeerBareAddress.h"
    "../../src/overlay/PeerDoor.cpp"
    "../../src/overlay/PeerDoor.h"
    "../../src/overlay/PeerManager.cpp"
    "../../src/overlay/PeerManager.h"
    "../../src/overlay/PeerSharedKeyId.cpp"
    "../../src/overlay/PeerSharedKeyId.h"
    "../../src/overlay/RandomPeerSource.cpp"
    "../../src/overlay/RandomPeerSource.h"
    "../../src/overlay/readme.md"
    "../../src/overlay/StellarXDR.h"
    "../../src/overlay/SurveyManager.cpp"
    "../../src/overlay/SurveyManager.h"
    "../../src/overlay/SurveyMessageLimiter.cpp"
    "../../src/overlay/SurveyMessageLimiter.h"
    "../../src/overlay/TCPPeer.cpp"
    "../../src/overlay/TCPPeer.h"
    "../../src/overlay/Tracker.cpp"
    "../../src/overlay/Tracker.h"
)
source_group("overlay" FILES ${overlay})

set(overlay__tests
    "../../src/overlay/test/FloodTests.cpp"
    "../../src/overlay/test/ItemFetcherTests.cpp"
    "../../src/overlay/test/LoopbackPeer.cpp"
    "../../src/overlay/test/LoopbackPeer.h"
    "../../src/overlay/test/OverlayManagerTests.cpp"
    "../../src/overlay/test/OverlayTests.cpp"
    "../../src/overlay/test/PeerManagerTests.cpp"
    "../../src/overlay/test/SurveyManagerTests.cpp"
    "../../src/overlay/test/SurveyMessageLimiterTests.cpp"
    "../../src/overlay/test/TCPPeerTests.cpp"
    "../../src/overlay/test/TrackerTests.cpp"
)
source_group("overlay\\tests" FILES ${overlay__tests})

set(process
    "../../src/process/ProcessManager.h"
    "../../src/process/ProcessManagerImpl.cpp"
    "../../src/process/ProcessManagerImpl.h"
)
source_group("process" FILES ${process})

set(process__tests
    "../../src/process/test/ProcessTests.cpp"
)
source_group("process\\tests" FILES ${process__tests})

set(scp
    "../../src/scp/BallotProtocol.cpp"
    "../../src/scp/BallotProtocol.h"
    "../../src/scp/LocalNode.cpp"
    "../../src/scp/LocalNode.h"
    "../../src/scp/NominationProtocol.cpp"
    "../../src/scp/NominationProtocol.h"
    "../../src/scp/QuorumSetUtils.cpp"
    "../../src/scp/QuorumSetUtils.h"
    "../../src/scp/readme.md"
    "../../src/scp/SCP.cpp"
    "../../src/scp/SCP.h"
    "../../src/scp/SCPDriver.cpp"
    "../../src/scp/SCPDriver.h"
    "../../src/scp/Slot.cpp"
    "../../src/scp/Slot.h"
)
source_group("scp" FILES ${scp})

set(scp__tests
    "../../src/scp/test/QuorumSetTests.cpp"
    "../../src/scp/test/SCPTests.cpp"
    "../../src/scp/test/SCPUnitTests.cpp"
)
source_group("scp\\tests" FILES ${scp__tests})

set(simulation
    "../../src/simulation/CoreTests.cpp"
    "../../src/simulation/LoadGenerator.cpp"
    "../../src/simulation/LoadGenerator.h"
    "../../src/simulation/Simulation.cpp"
    "../../src/simulation/Simulation.h"
    "../../src/simulation/test/LoadGeneratorTests.cpp"
    "../../src/simulation/Topologies.cpp"
    "../../src/simulation/Topologies.h"
)
source_group("simulation" FILES ${simulation})

set(test
    "../../src/test/fuzz.cpp"
    "../../src/test/fuzz.h"
    "../../src/test/Fuzzer.h"
    "../../src/test/FuzzerImpl.cpp"
    "../../src/test/FuzzerImpl.h"
    "../../src/test/SimpleTestReporter.h"
    "../../src/test/test.cpp"
    "../../src/test/test.h"
    "../../src/test/TestAccount.cpp"
    "../../src/test/TestAccount.h"
    "../../src/test/TestExceptions.cpp"
    "../../src/test/TestExceptions.h"
    "../../src/test/TestMarket.cpp"
    "../../src/test/TestMarket.h"
    "../../src/test/TestPrinter.cpp"
    "../../src/test/TestPrinter.h"
    "../../src/test/TestUtils.cpp"
    "../../src/test/TestUtils.h"
    "../../src/test/TxTests.cpp"
    "../../src/test/TxTests.h"
)
source_group("test" FILES ${test})

set(transactions
    "../../src/transactions/AllowTrustOpFrame.cpp"
    "../../src/transactions/AllowTrustOpFrame.h"
    "../../src/transactions/BeginSponsoringFutureReservesOpFrame.cpp"
    "../../src/transactions/BeginSponsoringFutureReservesOpFrame.h"
    "../../src/transactions/BumpSequenceOpFrame.cpp"
    "../../src/transactions/BumpSequenceOpFrame.h"
    "../../src/transactions/ChangeTrustOpFrame.cpp"
    "../../src/transactions/ChangeTrustOpFrame.h"
    "../../src/transactions/ClaimClaimableBalanceOpFrame.cpp"
    "../../src/transactions/ClaimClaimableBalanceOpFrame.h"
    "../../src/transactions/ClawbackClaimableBalanceOpFrame.cpp"
    "../../src/transactions/ClawbackClaimableBalanceOpFrame.h"
    "../../src/transactions/ClawbackOpFrame.cpp"
    "../../src/transactions/ClawbackOpFrame.h"
    "../../src/transactions/CreateAccountOpFrame.cpp"
    "../../src/transactions/CreateAccountOpFrame.h"
    "../../src/transactions/CreateClaimableBalanceOpFrame.cpp"
    "../../src/transactions/CreateClaimableBalanceOpFrame.h"
    "../../src/transactions/CreatePassiveSellOfferOpFrame.cpp"
    "../../src/transactions/CreatePassiveSellOfferOpFrame.h"
    "../../src/transactions/EndSponsoringFutureReservesOpFrame.cpp"
    "../../src/transactions/EndSponsoringFutureReservesOpFrame.h"
    "../../src/transactions/FeeBumpTransactionFrame.cpp"
    "../../src/transactions/FeeBumpTransactionFrame.h"
    "../../src/transactions/InflationOpFrame.cpp"
    "../../src/transactions/InflationOpFrame.h"
    "../../src/transactions/LiquidityPoolDepositOpFrame.cpp"
    "../../src/transactions/LiquidityPoolDepositOpFrame.h"
    "../../src/transactions/LiquidityPoolWithdrawOpFrame.cpp"
    "../../src/transactions/LiquidityPoolWithdrawOpFrame.h"
    "../../src/transactions/ManageBuyOfferOpFrame.cpp"
    "../../src/transactions/ManageBuyOfferOpFrame.h"
    "../../src/transactions/ManageDataOpFrame.cpp"
    "../../src/transactions/ManageDataOpFrame.h"
    "../../src/transactions/ManageOfferOpFrameBase.cpp"
    "../../src/transactions/ManageOfferOpFrameBase.h"
    "../../src/transactions/ManageSellOfferOpFrame.cpp"
    "../../src/transactions/ManageSellOfferOpFrame.h"
    "../../src/transactions/MergeOpFrame.cpp"
    "../../src/transactions/MergeOpFrame.h"
    "../../src/transactions/OfferExchange.cpp"
    "../../src/transactions/OfferExchange.h"
    "../../src/transactions/OperationFrame.cpp"
    "../../src/transactions/OperationFrame.h"
    "../../src/transactions/PathPaymentOpFrameBase.cpp"
    "../../src/transactions/PathPaymentOpFrameBase.h"
    "../../src/transactions/PathPaymentStrictReceiveOpFrame.cpp"
    "../../src/transactions/PathPaymentStrictReceiveOpFrame.h"
    "../../src/transactions/PathPaymentStrictSendOpFrame.cpp"
    "../../src/transactions/PathPaymentStrictSendOpFrame.h"
    "../../src/transactions/PaymentOpFrame.cpp"
    "../../src/transactions/PaymentOpFrame.h"
    "../../src/transactions/readme.md"
    "../../src/transactions/RevokeSponsorshipOpFrame.cpp"
    "../../src/transactions/RevokeSponsorshipOpFrame.h"
    "../../src/transactions/SetOptionsOpFrame.cpp"
    "../../src/transactions/SetOptionsOpFrame.h"
    "../../src/transactions/SetTrustLineFlagsOpFrame.cpp"
    "../../src/transactions/SetTrustLineFlagsOpFrame.h"
    "../../src/transactions/SignatureChecker.cpp"
    "../../src/transactions/SignatureChecker.h"
    "../../src/transactions/SignatureUtils.cpp"
    "../../src/transactions/SignatureUtils.h"
    "../../src/transactions/SponsorshipUtils.cpp"
    "../../src/transactions/SponsorshipUtils.h"
    "../../src/transactions/TransactionBridge.cpp"
    "../../src/transactions/TransactionBridge.h"
    "../../src/transactions/TransactionFrame.cpp"
    "../../src/transactions/TransactionFrame.h"
    "../../src/transactions/TransactionFrameBase.cpp"
    "../../src/transactions/TransactionFrameBase.h"
    "../../src/transactions/TransactionSQL.cpp"
    "../../src/transactions/TransactionSQL.h"
    "../../src/transactions/TransactionUtils.cpp"
    "../../src/transactions/TransactionUtils.h"
    "../../src/transactions/TrustFlagsOpFrameBase.cpp"
    "../../src/transactions/TrustFlagsOpFrameBase.h"
)
source_group("transactions" FILES ${transactions})

set(transactions__simulation
    "../../src/transactions/simulation/TxSimCreateClaimableBalanceOpFrame.cpp"
    "../../src/transactions/simulation/TxSimCreateClaimableBalanceOpFrame.h"
    "../../src/transactions/simulation/TxSimCreatePassiveSellOfferOpFrame.cpp"
    "../../src/transactions/simulation/TxSimCreatePassiveSellOfferOpFrame.h"
    "../../src/transactions/simulation/TxSimFeeBumpTransactionFrame.cpp"
    "../../src/transactions/simulation/TxSimFeeBumpTransactionFrame.h"
    "../../src/transactions/simulation/TxSimGenerateBucketsWork.cpp"
    "../../src/transactions/simulation/TxSimGenerateBucketsWork.h"
    "../../src/transactions/simulation/TxSimManageBuyOfferOpFrame.cpp"
    "../../src/transactions/simulation/TxSimManageBuyOfferOpFrame.h"
    "../../src/transactions/simulation/TxSimManageSellOfferOpFrame.cpp"
    "../../src/transactions/simulation/TxSimManageSellOfferOpFrame.h"
    "../../src/transactions/simulation/TxSimMergeOpFrame.cpp"
    "../../src/transactions/simulation/TxSimMergeOpFrame.h"
    "../../src/transactions/simulation/TxSimScaleBucketlistWork.cpp"
    "../../src/transactions/simulation/TxSimScaleBucketlistWork.h"
    "../../src/transactions/simulation/TxSimTransactionFrame.cpp"
    "../../src/transactions/simulation/TxSimTransactionFrame.h"
    "../../src/transactions/simulation/TxSimUtils.cpp"
    "../../src/transactions/simulation/TxSimUtils.h"
)
source_group("transactions\\simulation" FILES ${transactions__simulation})

set(transactions__tests
    "../../src/transactions/test/AllowTrustTests.cpp"
    "../../src/transactions/test/BeginSponsoringFutureReservesTests.cpp"
    "../../src/transactions/test/BumpSequenceTests.cpp"
    "../../src/transactions/test/ChangeTrustTests.cpp"
    "../../src/transactions/test/ClaimableBalanceTests.cpp"
    "../../src/transactions/test/ClawbackClaimableBalanceTests.cpp"
    "../../src/transactions/test/ClawbackTests.cpp"
    "../../src/transactions/test/CreateAccountTests.cpp"
    "../../src/transactions/test/EndSponsoringFutureReservesTests.cpp"
    "../../src/transactions/test/ExchangeTests.cpp"
    "../../src/transactions/test/FeeBumpTransactionTests.cpp"
    "../../src/transactions/test/InflationTests.cpp"
    "../../src/transactions/test/LiquidityPoolDepositTests.cpp"
    "../../src/transactions/test/LiquidityPoolTradeTests.cpp"
    "../../src/transactions/test/LiquidityPoolWithdrawTests.cpp"
    "../../src/transactions/test/ManageBuyOfferTests.cpp"
    "../../src/transactions/test/ManageDataTests.cpp"
    "../../src/transactions/test/MergeTests.cpp"
    "../../src/transactions/test/OfferTests.cpp"
    "../../src/transactions/test/PathPaymentStrictSendTests.cpp"
    "../../src/transactions/test/PathPaymentTests.cpp"
    "../../src/transactions/test/PaymentTests.cpp"
    "../../src/transactions/test/RevokeSponsorshipTests.cpp"
    "../../src/transactions/test/SetOptionsTests.cpp"
    "../../src/transactions/test/SetTrustLineFlagsTests.cpp"
    "../../src/transactions/test/SignatureUtilsTest.cpp"
    "../../src/transactions/test/SponsorshipTestUtils.cpp"
    "../../src/transactions/test/SponsorshipTestUtils.h"
    "../../src/transactions/test/TxEnvelopeTests.cpp"
    "../../src/transactions/test/TxResultsTests.cpp"
)
source_group("transactions\\tests" FILES ${transactions__tests})

set(util
    "../../lib/util/basen.h"
    "../../lib/util/cbitset.c"
    "../../lib/util/cbitset.h"
    "../../lib/util/cbitset_portability.h"
    "../../lib/util/crc16.cpp"
    "../../lib/util/crc16.h"
    "../../lib/util/stdrandom.h"
    "../../src/util/Algorithm.h"
    "../../src/util/asio.h"
    "../../src/util/Backtrace.cpp"
    "../../src/util/Backtrace.h"
    "../../src/util/BitSet.h"
    "../../src/util/Decoder.h"
    "../../src/util/FileSystemException.cpp"
    "../../src/util/FileSystemException.h"
    "../../src/util/Fs.cpp"
    "../../src/util/Fs.h"
    "../../src/util/GlobalChecks.cpp"
    "../../src/util/GlobalChecks.h"
    "../../src/util/HashOfHash.cpp"
    "../../src/util/HashOfHash.h"
    "../../src/util/Logging.cpp"
    "../../src/util/Logging.h"
    "../../src/util/LogPartitions.def"
    "../../src/util/LogSlowExecution.cpp"
    "../../src/util/LogSlowExecution.h"
    "../../src/util/make_unique.h"
    "../../src/util/Math.cpp"
    "../../src/util/Math.h"
    "../../src/util/MetricResetter.cpp"
    "../../src/util/MetricResetter.h"
    "../../src/util/must_use.h"
    "../../src/util/NonCopyable.h"
    "../../src/util/numeric.cpp"
    "../../src/util/numeric.h"
    "../../src/util/numeric128.h"
    "../../src/util/RandHasher.cpp"
    "../../src/util/RandHasher.h"
    "../../src/util/RandomEvictionCache.h"
    "../../src/util/Scheduler.cpp"
    "../../src/util/Scheduler.h"
    "../../src/util/SecretValue.cpp"
    "../../src/util/SecretValue.h"
    "../../src/util/SociNoWarnings.h"
    "../../src/util/SpdlogTweaks.h"
    "../../src/util/StatusManager.cpp"
    "../../src/util/StatusManager.h"
    "../../src/util/TarjanSCCCalculator.cpp"
    "../../src/util/TarjanSCCCalculator.h"
    "../../src/util/Thread.cpp"
    "../../src/util/Thread.h"
    "../../src/util/Timer.cpp"
    "../../src/util/Timer.h"
    "../../src/util/TmpDir.cpp"
    "../../src/util/TmpDir.h"
    "../../src/util/types.cpp"
    "../../src/util/types.h"
    "../../src/util/UnorderedMap.h"
    "../../src/util/UnorderedSet.h"
    "../../src/util/XDRCereal.cpp"
    "../../src/util/XDRCereal.h"
    "../../src/util/XDROperators.h"
    "../../src/util/XDRStream.h"
)
source_group("util" FILES ${util})

set(util__tests
    "../../src/util/test/BalanceTests.cpp"
    "../../src/util/test/BigDivideTests.cpp"
    "../../src/util/test/BitSetTests.cpp"
    "../../src/util/test/CacheTests.cpp"
    "../../src/util/test/DecoderTests.cpp"
    "../../src/util/test/FsTests.cpp"
    "../../src/util/test/MathTests.cpp"
    "../../src/util/test/MetricTests.cpp"
    "../../src/util/test/SchedulerTests.cpp"
    "../../src/util/test/StatusManagerTest.cpp"
    "../../src/util/test/TimerTests.cpp"
    "../../src/util/test/Uint128Tests.cpp"
    "../../src/util/test/XDRStreamTests.cpp"
)
source_group("util\\tests" FILES ${util__tests})

set(work
    "../../src/work/BasicWork.cpp"
    "../../src/work/BasicWork.h"
    "../../src/work/BatchWork.cpp"
    "../../src/work/BatchWork.h"
    "../../src/work/ConditionalWork.cpp"
    "../../src/work/ConditionalWork.h"
    "../../src/work/Work.cpp"
    "../../src/work/Work.h"
    "../../src/work/WorkScheduler.cpp"
    "../../src/work/WorkScheduler.h"
    "../../src/work/WorkSequence.cpp"
    "../../src/work/WorkSequence.h"
    "../../src/work/WorkWithCallback.cpp"
    "../../src/work/WorkWithCallback.h"
)
source_group("work" FILES ${work})

set(work__tests
    "../../src/work/test/WorkTests.cpp"
)
source_group("work\\tests" FILES ${work__tests})

set(xdr
    "../../src/xdr/Stellar-ledger-entries.x"
    "../../src/xdr/Stellar-ledger.x"
    "../../src/xdr/Stellar-overlay.x"
    "../../src/xdr/Stellar-SCP.x"
    "../../src/xdr/Stellar-transaction.x"
    "../../src/xdr/Stellar-types.x"
)
source_group("xdr" FILES ${xdr})

set(xdr__generated
    "src/generated/xdr/Stellar-ledger-entries.h"
    "src/generated/xdr/Stellar-ledger.h"
    "src/generated/xdr/Stellar-overlay.h"
    "src/generated/xdr/Stellar-SCP.h"
    "src/generated/xdr/Stellar-transaction.h"
    "src/generated/xdr/Stellar-types.h"
)
source_group("xdr\\generated" FILES ${xdr__generated})

set(ALL_FILES
    ${no_group_source_files}
    ${bucket}
    ${bucket__tests}
    ${catchup}
    ${catchup__simulation}
    ${catchup__tests}
    ${crypto}
    ${crypto__tests}
    ${database}
    ${database__tests}
    ${docs}
    ${docs__software}
    ${github}
    ${herder}
    ${herder__docs}
    ${herder__simulation}
    ${herder__tests}
    ${history}
    ${historyWork}
    ${historyWork__tests}
    ${history__tests}
    ${history__tests__serialize-tests}
    ${invariant}
    ${invariant__tests}
    ${ledger}
    ${ledger__tests}
    ${lib}
    ${lib__fmt}
    ${lib__http}
    ${lib__json}
    ${lib__spdlog}
    ${lib__tracy}
    ${lib__util}
    ${lib__xdrpp}
    ${main}
    ${main__generated}
    ${main__tests}
    ${overlay}
    ${overlay__tests}
    ${process}
    ${process__tests}
    ${scp}
    ${scp__tests}
    ${simulation}
    ${test}
    ${transactions}
    ${transactions__simulation}
    ${transactions__tests}
    ${util}
    ${util__tests}
    ${work}
    ${work__tests}
    ${xdr}
    ${xdr__generated}
)

################################################################################
# Target
################################################################################
add_executable(${PROJECT_NAME} ${ALL_FILES})

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
set(ROOT_NAMESPACE stellar-core)

################################################################################
# Output directory
################################################################################
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_DIRECTORY_DEBUG           "${CMAKE_SOURCE_DIR}/${CMAKE_VS_PLATFORM_NAME}/$<CONFIG>/"
    OUTPUT_DIRECTORY_DEBUGNOPOSTGRES "${CMAKE_SOURCE_DIR}/${CMAKE_VS_PLATFORM_NAME}/$<CONFIG>/"
    OUTPUT_DIRECTORY_RELEASE         "${CMAKE_SOURCE_DIR}/${CMAKE_VS_PLATFORM_NAME}/$<CONFIG>/"
)
set_target_properties(${PROJECT_NAME} PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE         "TRUE"
)
################################################################################
# MSVC runtime library
################################################################################
get_property(MSVC_RUNTIME_LIBRARY_DEFAULT TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY)
string(CONCAT "MSVC_RUNTIME_LIBRARY_STR"
    $<$<CONFIG:Debug>:
        MultiThreadedDebugDLL
    >
    $<$<CONFIG:DebugNoPostgres>:
        MultiThreadedDebugDLL
    >
    $<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:DebugNoPostgres>>>:${MSVC_RUNTIME_LIBRARY_DEFAULT}>
)
set_target_properties(${PROJECT_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY ${MSVC_RUNTIME_LIBRARY_STR})

################################################################################
# Include directories
################################################################################
target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/tracy;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/spdlog/include;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/libmedida/src;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/soci/src/core;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/autocheck/include;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/cereal/include;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/asio/asio/include;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/xdrpp;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/libsodium/src/libsodium/include;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/fmt/include;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../..;"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/sqlite;"
    "c:/Program Files/PostgreSQL/9.5/include"
)

################################################################################
# Compile definitions
################################################################################
target_compile_definitions(${PROJECT_NAME} PRIVATE
    "$<$<CONFIG:Debug>:"
        "USE_POSTGRES"
    ">"
    "$<$<CONFIG:Release>:"
        "USE_POSTGRES"
    ">"
    "ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION=1;"
    "USE_SPDLOG;"
    "FMT_HEADER_ONLY=1;"
    "BUILD_TESTS;"
    "WIN32_LEAN_AND_MEAN;"
    "NOMINMAX;"
    "ASIO_STANDALONE;"
    "_WINSOCK_DEPRECATED_NO_WARNINGS;"
    "SODIUM_STATIC;"
    "ASIO_SEPARATE_COMPILATION;"
    "ASIO_ERROR_CATEGORY_NOEXCEPT=noexcept;"
    "TRACY_ENABLE;"
    "TRACY_ON_DEMAND;"
    "TRACY_NO_BROADCAST;"
    "TRACY_ONLY_LOCALHOST;"
    "USE_TRACY;"
    "_CRT_SECURE_NO_WARNINGS;"
    "_WIN32_WINNT=0x0601;"
    "WIN32;"
    "_MBCS;"
    "_CRT_NONSTDC_NO_DEPRECATE"
)

################################################################################
# Compile and link options
################################################################################
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:
            /Gm-;
            /Od;
            /Zi
        >
        $<$<CONFIG:DebugNoPostgres>:
            /Gm-;
            /Od;
            /Zi
        >
        $<$<CONFIG:Release>:
            /O2;
            /Oi;
            /Oy-;
            /Gy;
            ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT}
        >
        /MP;
        /std:c++17;
        /sdl;
        /W4;
        /wd4060;
        /wd4100;
        /wd4127;
        /wd4324;
        /wd4408;
        /wd4510;
        /wd4512;
        /wd4582;
        /wd4583;
        /wd4592;
        /bigobj;
        ${DEFAULT_CXX_EXCEPTION_HANDLING}
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:
            /DEBUG:FASTLINK;
            /SUBSYSTEM:CONSOLE
        >
        $<$<CONFIG:DebugNoPostgres>:
            /DEBUG:FASTLINK
        >
        $<$<CONFIG:Release>:
            /DEBUG;
            /OPT:REF;
            /OPT:ICF
        >
    )
endif()

################################################################################
# Post build events
################################################################################
add_custom_command_if(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMANDS
    COMMAND           $<CONFIG:Debug> robocopy $ENV{MSBuildProjectDirectory}\\..\\..\\docs $<SHELL_PATH:${OUTPUT_DIRECTORY}>\\testdata *.cfg > nul
    COMMAND           $<CONFIG:Debug> robocopy $ENV{MSBuildProjectDirectory}\\..\\..\\src\\history\\serialize-tests $<SHELL_PATH:${OUTPUT_DIRECTORY}>\\testdata *.json > nul
    COMMAND           $<CONFIG:Debug> exit /b 0
    COMMAND $<CONFIG:DebugNoPostgres> robocopy $ENV{MSBuildProjectDirectory}\\..\\..\\docs $<SHELL_PATH:${OUTPUT_DIRECTORY}>\\testdata *.cfg > nul
    COMMAND $<CONFIG:DebugNoPostgres> robocopy $ENV{MSBuildProjectDirectory}\\..\\..\\src\\history\\serialize-tests $<SHELL_PATH:${OUTPUT_DIRECTORY}>\\testdata *.json > nul
    COMMAND $<CONFIG:DebugNoPostgres> exit /b 0
    COMMAND         $<CONFIG:Release> robocopy $ENV{MSBuildProjectDirectory}\\..\\..\\docs $<SHELL_PATH:${OUTPUT_DIRECTORY}>\\testdata *.cfg > nul
    COMMAND         $<CONFIG:Release> robocopy $ENV{MSBuildProjectDirectory}\\..\\..\\src\\history\\serialize-tests $<SHELL_PATH:${OUTPUT_DIRECTORY}>\\testdata *.json > nul
    COMMAND         $<CONFIG:Release> exit /b 0
    COMMENT "copy testdata"
)

################################################################################
# Custom build events
################################################################################
add_custom_command_if(
    OUTPUT "$(IntermediateOutputPath)\xdrgen.stamp"
    COMMANDS
    COMMAND           $<CONFIG:Debug> @echo XDR > $ENV{IntermediateOutputPath}\\xdrgen.stamp
    COMMAND $<CONFIG:DebugNoPostgres> @echo XDR > $ENV{IntermediateOutputPath}\\xdrgen.stamp
    COMMAND         $<CONFIG:Release> @echo XDR > $ENV{IntermediateOutputPath}\\xdrgen.stamp
    COMMENT "XDR check"
)

################################################################################
# Dependencies
################################################################################
# Link with other targets.
target_link_libraries(${PROJECT_NAME} PRIVATE
    libsodium
    xdrc
    xdrpp
    libmedida
    libsoci
    libsoci_postgresql
    libsoci_sqlite3
    libsqlite
)

set(ADDITIONAL_LIBRARY_DEPENDENCIES
    "$<$<CONFIG:Debug>:"
        "C:\Program Files\PostgreSQL\9.5\lib\libpq"
    ">"
    "$<$<CONFIG:Release>:"
        "C:\Program Files\PostgreSQL\9.5\lib\libpq"
    ">"
    "kernel32;"
    "user32;"
    "gdi32;"
    "winspool;"
    "comdlg32;"
    "advapi32;"
    "shell32;"
    "ole32;"
    "oleaut32;"
    "uuid;"
    "odbc32;"
    "odbccp32;"
    "psapi;"
    "Credui"
)
target_link_libraries(${PROJECT_NAME} PRIVATE "${ADDITIONAL_LIBRARY_DEPENDENCIES}")

