// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/types.h"
#include "util/xdrquery/XDRFieldResolver.h"
#include "util/xdrquery/XDRMatcher.h"
#include "xdr/Stellar-ledger-entries.h"

#include <algorithm>
#include <lib/catch.hpp>

namespace xdrquery
{
namespace
{
using namespace stellar;

LedgerEntry
makeAccountEntry(int64_t balance)
{
    LedgerEntry accountEntry;
    accountEntry.data.type(ACCOUNT);
    auto& account = accountEntry.data.account();
    account.accountID.ed25519().back() = 111;
    account.balance = balance;
    account.seqNum = std::numeric_limits<int64_t>::max();
    account.numSubEntries = std::numeric_limits<uint32_t>::min();
    account.inflationDest.activate().ed25519()[0] = 78;
    account.homeDomain = "home_domain";
    account.thresholds[0] = 1;
    account.thresholds[2] = 2;
    account.ext.v(1);
    account.ext.v1().liabilities.buying = std::numeric_limits<int64_t>::min();
    account.ext.v1().ext.v(2);
    account.ext.v1().ext.v2().ext.v(3);
    account.ext.v1().ext.v2().ext.v3().seqTime =
        std::numeric_limits<uint64_t>::max();
    return accountEntry;
}

LedgerEntry
makeDataEntry(std::string const& dataName)
{
    LedgerEntry dataEntry;
    dataEntry.data.type(DATA);
    dataEntry.data.data().accountID.ed25519().back() = 111;
    dataEntry.data.data().dataName = dataName;
    return dataEntry;
}

TEST_CASE("XDR field resolver", "[xdrquery]")
{
    auto accountEntry = makeAccountEntry(123);
    auto const& account = accountEntry.data.account();

    SECTION("int32 field")
    {
        Price price;
        price.n = std::numeric_limits<int32_t>::min();
        price.d = std::numeric_limits<int32_t>::max();
        SECTION("negative")
        {
            auto field = getXDRField(price, {"n"});
            REQUIRE(std::get<int32_t>(*field) == price.n);
        }
        SECTION("positive")
        {
            auto field = getXDRField(price, {"d"});
            REQUIRE(std::get<int32_t>(*field) == price.d);
        }
    }

    SECTION("uint32 field")
    {
        auto field =
            getXDRField(accountEntry, {"data", "account", "numSubEntries"});
        REQUIRE(std::get<uint32_t>(*field) == account.numSubEntries);
    }

    SECTION("int64 field")
    {
        SECTION("negative")
        {
            auto field =
                getXDRField(accountEntry, {"data", "account", "ext", "v1",
                                           "liabilities", "buying"});
            REQUIRE(std::get<int64_t>(*field) ==
                    account.ext.v1().liabilities.buying);
        }
        SECTION("positive")
        {
            auto field =
                getXDRField(accountEntry, {"data", "account", "seqNum"});
            REQUIRE(std::get<int64_t>(*field) == account.seqNum);
        }
    }

    SECTION("uint64 field")
    {
        auto field =
            getXDRField(accountEntry, {"data", "account", "ext", "v1", "ext",
                                       "v2", "ext", "v3", "seqTime"});
        REQUIRE(std::get<uint64_t>(*field) ==
                account.ext.v1().ext.v2().ext.v3().seqTime);
    }

    SECTION("string field")
    {
        auto field =
            getXDRField(accountEntry, {"data", "account", "homeDomain"});
        REQUIRE(std::get<std::string>(*field) == account.homeDomain);
    }

    SECTION("bytes field")
    {
        auto field =
            getXDRField(accountEntry, {"data", "account", "thresholds"});
        REQUIRE(std::get<std::string>(*field) == "01000200");
    }

    SECTION("enum field")
    {
        auto field = getXDRField(accountEntry, {"data", "type"});
        REQUIRE(std::get<std::string>(*field) == "ACCOUNT");
    }

    SECTION("public key field")
    {
        SECTION("non-optional")
        {
            auto field =
                getXDRField(accountEntry, {"data", "account", "accountID"});
            REQUIRE(std::get<std::string>(*field) ==
                    "GAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAG6ELY");
        }
        SECTION("optional")
        {
            auto field =
                getXDRField(accountEntry, {"data", "account", "inflationDest"});
            REQUIRE(std::get<std::string>(*field) ==
                    "GBHAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB2HL");
        }
    }

    SECTION("asset field")
    {
        auto testAsset = [&](auto& entry, auto& asset,
                             std::vector<std::string> const& fieldPath) {
            SECTION("native")
            {
                asset.type(ASSET_TYPE_NATIVE);

                auto field = getXDRField(entry, fieldPath);
                REQUIRE(std::get<std::string>(*field) == "NATIVE");
            }
            auto testAlphaNum = [&](auto& alphaNum, std::string const& code) {
                strToAssetCode(alphaNum.assetCode, code);
                std::copy(account.accountID.ed25519().begin(),
                          account.accountID.ed25519().end(),
                          alphaNum.issuer.ed25519().begin());
                SECTION("assetCode")
                {
                    auto currFieldPath = fieldPath;
                    currFieldPath.push_back("assetCode");
                    auto field = getXDRField(entry, currFieldPath);
                    REQUIRE(std::get<std::string>(*field) == code);
                }

                SECTION("issuer")
                {
                    auto currFieldPath = fieldPath;
                    currFieldPath.push_back("issuer");
                    auto field = getXDRField(entry, currFieldPath);
                    REQUIRE(std::get<std::string>(*field) ==
                            "GAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                            "AG6ELY");
                }
            };
            SECTION("alphanum4")
            {
                asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
                testAlphaNum(asset.alphaNum4(), "USD");
            }
            SECTION("alphanum12")
            {
                asset.type(ASSET_TYPE_CREDIT_ALPHANUM12);
                testAlphaNum(asset.alphaNum12(), "USD123");
            }
        };
        SECTION("regular asset")
        {
            OfferEntry entry;
            testAsset(entry, entry.selling, {"selling"});
        }
        SECTION("trustline asset")
        {
            TrustLineEntry entry;
            testAsset(entry, entry.asset, {"asset"});

            SECTION("pool share")
            {
                entry.asset.type(ASSET_TYPE_POOL_SHARE);
                entry.asset.liquidityPoolID()[0] = 1;
                entry.asset.liquidityPoolID()[2] = 2;
                auto field = getXDRField(entry, {"asset", "liquidityPoolID"});
                REQUIRE(std::get<std::string>(*field) ==
                        "010002000000000000000000000000000000000000000000000000"
                        "0000000000");
            }
        }
    }

    SECTION("non-matching paths return nullopt")
    {
        SECTION("bad path")
        {
            auto field =
                getXDRField(accountEntry, {"data", "account", "noSuchField"});
            REQUIRE(!field);
        }
        SECTION("wrong union element")
        {
            auto field =
                getXDRField(accountEntry, {"data", "trustLine", "accountID"});
            REQUIRE(!field);
        }
    }

    SECTION("bad paths throw exception")
    {
        SECTION("leaf field in the middle")
        {
            REQUIRE_THROWS_AS(
                getXDRField(accountEntry,
                            {"data", "account", "balance", "balance2"}),
                XDRQueryError);
        }
        SECTION("non-leaf field in the end")
        {
            REQUIRE_THROWS_AS(getXDRField(accountEntry, {"data", "account"}),
                              XDRQueryError);
        }
    }
}

TEST_CASE("XDR matcher", "[xdrquery]")
{
    std::vector<LedgerEntry> entries = {
        makeAccountEntry(100), makeAccountEntry(200), makeDataEntry("foo"),
        makeDataEntry("foobar")};
    auto testMatches = [&](std::string const& query,
                           std::vector<bool> const& expectedMatches) {
        XDRMatcher matcher(query);
        for (int i = 0; i < expectedMatches.size(); ++i)
        {
            REQUIRE(matcher.matchXDR(entries[i]) == expectedMatches[i]);
        }
    };

    SECTION("single comparison")
    {
        SECTION("ints")
        {
            testMatches("data.account.balance == 100", {true, false});
            testMatches("100 != data.account.balance", {false, true});
            testMatches("data.account.balance < 150", {true, false});
            testMatches("data.account.balance <= 100", {true, false});
            testMatches("data.account.balance > 150", {false, true});
            testMatches("200 >= data.account.balance", {true, true});
        }

        SECTION("strings")
        {
            testMatches("data.type == 'ACCOUNT'", {true, true, false, false});
            testMatches("data.type != 'ACCOUNT'", {false, false, true, true});
            testMatches("data.data.dataName < 'foobar'",
                        {false, false, true, false});
            testMatches("data.data.dataName <= 'foo'",
                        {false, false, true, false});
            testMatches("data.data.dataName > 'foo'",
                        {false, false, false, true});
            testMatches("data.data.dataName >= 'foo'",
                        {false, false, true, true});
        }
    }

    SECTION("queries with operators")
    {
        SECTION("or operator")
        {
            testMatches(R"(
                data.account.accountID == "GAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAG6ELY" 
                || data.data.accountID == "GAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAG6ELY"
            )",
                        {true, true, true, true});
            testMatches(
                "data.account.balance > 150 || data.data.dataName == 'foo'",
                {false, true, true, false});
        }

        SECTION("and operator")
        {
            testMatches(R"(data.account.balance > 150 
                           && '01000200' ==  data.account.thresholds)",
                        {false, true, false, false});
            testMatches("data.data.dataName == 'foo' && data.type != 'OFFER'",
                        {false, false, true, false});
        }

        SECTION("mixed operators")
        {
            testMatches(R"(data.type != 'OFFER' && 
                           ("01000200" == data.account.thresholds ||
                            data.data.dataName <= 'foo'))",
                        {true, true, true, false});
            testMatches(R"("01000200" == data.account.thresholds ||
                           data.type != 'OFFER' && 
                           data.data.dataName <= 'foo')",
                        {true, true, true, false});

            testMatches(R"("01000200" == data.account.thresholds &&
                           data.type != 'OFFER' && 
                           data.data.dataName <= 'foo')",
                        {false, false, false, false});
            testMatches(R"("01000200" == data.account.thresholds ||
                           data.type != 'OFFER' || 
                           data.data.dataName <= 'foo')",
                        {true, true, true, true});
        }
    }

    auto runQuery = [&](std::string const& query) {
        XDRMatcher matcher(query);
        matcher.matchXDR(entries[0]);
    };
    SECTION("query errors")
    {
        SECTION("syntax error")
        {
            REQUIRE_THROWS_AS(runQuery("data.type == 'ACCOUNT"), XDRQueryError);
            REQUIRE_THROWS_AS(runQuery("data.type = 'ACCOUNT'"), XDRQueryError);
            REQUIRE_THROWS_AS(runQuery("$data.type == 'ACCOUNT'"),
                              XDRQueryError);
        }

        SECTION("field error")
        {
            REQUIRE_THROWS_AS(runQuery("data.type.foo == 'ACCOUNT'"),
                              XDRQueryError);
            REQUIRE_THROWS_AS(runQuery("data.account == 'ACCOUNT'"),
                              XDRQueryError);
        }

        SECTION("int out of range")
        {
            REQUIRE_THROWS_AS(
                runQuery("data.account.balance <= 10000000000000000000"),
                XDRQueryError);
            REQUIRE_THROWS_AS(
                runQuery("5000000000 > data.account.numSubEntries"),
                XDRQueryError);
        }
    }
}

} // namespace
} // namespace xdrquery