// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <exception>
#include <variant>

#include "xdrpp/marshal.h"
#include "xdrpp/types.h"

namespace xdrquery
{
namespace internal
{
using namespace xdr;

struct XDRFieldCollector
{
    using FieldType =
        std::variant<bool, int32_t, uint32_t, int64_t, uint64_t, std::string>;

    XDRFieldCollector(std::vector<std::string> const& fieldPath)
        : mFieldPath(fieldPath), mPathIter(mFieldPath.cbegin())
    {
    }

    FieldType const&
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

    template <uint32_t N>
    void
    operator()(xstring<N> const& t, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = std::string(t);
        }
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_bytes>::type
    operator()(T const& t, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            throw XDRQueryError("Matching byte fields is not supported.");
        }
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_enum>::type
    operator()(T const& t, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = std::string(xdr_traits<T>::enum_name(t));
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
            throw XDRQueryError(
                "Field path must end with a primitive field.");
        }
        xdr_traits<T>::save(*this, t);
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

    std::vector<std::string> const& mFieldPath;
    std::vector<std::string>::const_iterator mPathIter;
    FieldType mResult;
};

template <> struct archive_adapter<XDRFieldCollector>
{
    template <typename T>
    static void
    apply(XDRFieldCollector& ar, T&& t, char const* fieldName)
    {
        ar(std::forward<T>(t), fieldName);
    }
};
}

template <typename T> bool matchXdr(T const& xdrMessage)
{

}
}
