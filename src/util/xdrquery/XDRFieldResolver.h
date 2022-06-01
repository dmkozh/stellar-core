// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <exception>
#include <variant>

#include "util/types.h"
#include "util/xdrquery/XDRQueryError.h"
#include "util/xdrquery/XDRQueryEval.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdrpp/marshal.h"
#include "xdrpp/types.h"

namespace xdrquery
{
using namespace xdr;
using namespace stellar;

struct XDRFieldResolver
{
    XDRFieldResolver(std::vector<std::string> const& fieldPath)
        : mFieldPath(fieldPath)
        , mPathIter(mFieldPath.cbegin())        
    {
    }

    ResultType const&
    getResult()
    {
        return mResult;
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_numeric &&
                            !xdr_traits<T>::is_enum>::type
    operator()(T const& t, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = t;
        }
    }

    // Compare enums by their stringified names.
    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_enum>::type
    operator()(T const& t, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = std::string(xdr_traits<T>::enum_name(t));
        }
    }

    // Allow public key comparisons by their string representation.
    template <typename T>
    typename std::enable_if<std::is_same<PublicKey, T>::value>::type
    operator()(T const& k, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = KeyUtils::toStrKey<PublicKey>(k);
        }
    }

    // Allow comparisons like field.account == "<account_id_str>" (bypassing
    // account types).
    template <typename T>
    typename std::enable_if<std::is_same<MuxedAccount, T>::value>::type
    operator()(T const& muxedAccount, char const* fieldName)
    {
        bool isLeaf;
        if (checkMaybeLeafField(fieldName, isLeaf))
        {
            if (isLeaf)
            {
                mResult = KeyUtils::toStrKey(toAccountID(muxedAccount));
            }
            else
            {
                xdr_traits<T>::save(*this, muxedAccount);
            }
        }
    }

    template <typename T>
    typename std::enable_if<std::is_same<Asset, T>::value ||
                            std::is_same<TrustLineAsset, T>::value>::type
    operator()(T const& asset, char const* fieldName)
    {
        if (!matchFieldToPath(fieldName))
        {
            return;
        }
        ++mPathIter;
        switch (asset.type())
        {
        case ASSET_TYPE_NATIVE:
            if (mPathIter == mFieldPath.end())
            {
                // If non-leaf field is requested, then we're looking for
                // non-native asset.
                mResult = "NATIVE";
            }
            break;
        case ASSET_TYPE_POOL_SHARE:
            processPoolAsset(asset);
            break;
        case ASSET_TYPE_CREDIT_ALPHANUM4:
        case ASSET_TYPE_CREDIT_ALPHANUM12:
        {
            std::string code;
            if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
            {
                stellar::assetCodeToStr(asset.alphaNum4().assetCode, code);
            }
            else
            {
                stellar::assetCodeToStr(asset.alphaNum12().assetCode, code);
            }

            processString(code, "assetCode");
            *this(stellar::getIssuer(asset), "issuer");
            break;
        }
        default:
            mResult = "UNKNOWN";
            break;
        }
    }

    template <uint32_t N>
    void
    operator()(xstring<N> const& t, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = std::string(t);
        }
    }

    template <uint32_t N>
    void
    operator()(xdr::opaque_vec<N> const& t, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = binToHex(ByteSlice(s.data(), s.size()));
        }
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_container>::type
    operator()(T const& t, char const* fieldName)
    {
        if (matchFieldToPath(fieldName))
        {
            throw XDRQueryError("Array fields are not supported.");
        }
    }

    template <typename T>
    typename std::enable_if<!xdr_traits<T>::is_container &&
                            (xdr_traits<T>::is_union ||
                             xdr_traits<T>::is_class)>::type
    operator()(T const& t, char const* fieldName)
    {
        if (!matchFieldToPath(fieldName))
        {
            // Archive is first called with an empty 'virtual' XDR field
            // representing the whole struct.
            if (fieldName == nullptr && mPathIter == mFieldPath.begin())
            {
                xdr_traits<T>::save(*this, t);
            }
            return;
        }
        if (++mPathIter == mFieldPath.end())
        {
            throw XDRQueryError("Field path must end with a primitive field.");
        }
        xdr_traits<T>::save(*this, t);
    }

    template <typename T>
    void
    operator()(pointer<T> const& ptr, char const* fieldName)
    {
        if (ptr != nullptr)
        {
            archive(*this, *ptr, fieldName);
        }
        else
        {
            if (checkLeafField(fieldName))
            {
                mResult = NULL_LITERAL;
                return;
            }
        }
    }

  private:
    bool
    matchFieldToPath(char const* fieldName) const
    {
        return fieldName != nullptr && mPathIter != mFieldPath.end() &&
               *mPathIter == fieldName;
    }

    bool
    checkLeafField(char const* fieldName)
    {
        if (!matchFieldToPath(fieldName))
        {
            return false;
        }
        if (++mPathIter != mFieldPath.end())
        {
            throw XDRQueryError(
                "Encountered primitive field in the middle of the path.");
        }
        return true;
    }

    bool
    checkMaybeLeafField(char const* fieldName, bool& isLeaf)
    {
        if (!matchFieldToPath(fieldName))
        {
            return false;
        }
        return ++mPathIter != mFieldPath.end();
    }

    void
    processString(std::string const& s, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = s;
        }
    }

    void
    processPoolAsset(Asset const& asset)
    {
        throw std::runtime_error("Unexpected asset type for the pool asset.");
    }

    void
    processPoolAsset(TrustLineAsset const& asset)
    {
        //this->operator()(asset.liquidityPoolID(), "liquidityPoolID");
    }

    std::vector<std::string> const& mFieldPath;
    std::vector<std::string>::const_iterator mPathIter;
    ResultType mResult;
};
}

namespace xdr
{
template <> struct archive_adapter<xdrquery::XDRFieldResolver>
{
    template <typename T>
    static void
    apply(xdrquery::XDRFieldResolver& ar, T&& t, char const* fieldName)
    {
        ar(std::forward<T>(t), fieldName);
    }
};
}