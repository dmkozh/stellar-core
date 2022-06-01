// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/xdrquery/XDRQueryEval.h"
#include "fmt/format.h"
#include "util/xdrquery/XDRQueryError.h"

namespace xdrquery
{

LiteralNode::LiteralNode(LiteralNodeType valueType, std::string const& val)
    : mType(valueType), mValue(val)
{
    if (mType == LiteralNodeType::NULL_LITERAL)
    {
        mValue = NULL_LITERAL;
    }
}

ResultType
LiteralNode::eval(FieldResolver const& fieldResolver) const
{
    return mValue;
}

EvalNodeType
LiteralNode::getType() const
{
    return EvalNodeType::LITERAL;
}

void
LiteralNode::resolveIntType(ResultValueType const& fieldValue,
                            std::vector<std::string> const& fieldPath) const
{
    if (std::holds_alternative<std::string>(fieldValue))
    {
        std::string valueStr =
            std::visit([](auto&& v) { return fmt::to_string(v); }, *mValue);
        throw XDRQueryError(fmt::format(
            FMT_STRING("String field '{}' is compared with int value: {}."),
            fmt::join(fieldPath, "."), valueStr));
    }
    std::string valueStr = std::get<std::string>(*mValue);
    try
    {
        mValue = std::visit(
            [&valueStr](auto&& v) -> ResultType {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int32_t>)
                    return std::stoi(valueStr);
                else if constexpr (std::is_same_v<T, int64_t>)
                    return std::stoll(valueStr);
                else if constexpr (std::is_same_v<T, uint32_t>)
                    return static_cast<uint32_t>(std::stoul(valueStr));
                else if constexpr (std::is_same_v<T, uint64_t>)
                    return std::stoull(valueStr);
                throw std::runtime_error("Unexpected field type.");
            },
            fieldValue);
    }
    catch (std::out_of_range&)
    {
        throw XDRQueryError(fmt::format(
            FMT_STRING("Value for field '{}' is out of type range: {}."),
            fmt::join(fieldPath, "."), valueStr));
    }
}

FieldNode::FieldNode(std::string const& initField)
{
    mFieldPath.push_back(initField);
}

ResultType
FieldNode::eval(FieldResolver const& fieldResolver) const
{
    return fieldResolver(mFieldPath);
}

EvalNodeType
FieldNode::getType() const
{
    return EvalNodeType::FIELD;
}

ResultType
BoolEvalNode::eval(FieldResolver const& fieldResolver) const
{
    return evalBool(fieldResolver);
}

BoolOpNode::BoolOpNode(BoolOpNodeType nodeType,
                       std::unique_ptr<BoolEvalNode> left,
                       std::unique_ptr<BoolEvalNode> right)
    : mType(nodeType), mLeft(std::move(left)), mRight(std::move(right))
{
}

bool
BoolOpNode::evalBool(FieldResolver const& fieldResolver) const
{
    switch (mType)
    {
    case BoolOpNodeType::AND:
        return mLeft->evalBool(fieldResolver) &&
               mRight->evalBool(fieldResolver);
    case BoolOpNodeType::OR:
        return mLeft->evalBool(fieldResolver) ||
               mRight->evalBool(fieldResolver);
    }
}

EvalNodeType
BoolOpNode::getType() const
{
    return EvalNodeType();
}

ComparisonNode::ComparisonNode(ComparisonNodeType nodeType,
                               std::unique_ptr<EvalNode> left,
                               std::unique_ptr<EvalNode> right)
    : mType(nodeType), mLeft(std::move(left)), mRight(std::move(right))
{
    // Keep the field as the left argument for simplicity of type check during
    // evaluation.
    if (mRight->getType() == EvalNodeType::FIELD)
    {
        std::swap(mLeft, mRight);
    }
}

bool
ComparisonNode::evalBool(FieldResolver const& fieldResolver) const
{
    auto leftType = mLeft->getType();
    auto leftVal = mLeft->eval(fieldResolver);

    if (!leftVal)
    {
        return false;
    }

    auto rightType = mRight->getType();
    if (leftType == EvalNodeType::FIELD && rightType == EvalNodeType::LITERAL)
    {
        // Lazily resolve the type of the int literal using the field type.
        // This allows to correctly check the literal range and simplifies the
        // comparisons.
        auto* lit = static_cast<LiteralNode const*>(mRight.get());
        if (lit->mType == LiteralNodeType::INT &&
            std::holds_alternative<std::string>(*lit->mValue))
        {
            auto* field = static_cast<FieldNode const*>(mLeft.get());
            lit->resolveIntType(*leftVal, field->mFieldPath);
        }
    }
    auto rightVal = mRight->eval(fieldResolver);
    if (!rightVal)
    {
        return false;
    }

    switch (mType)
    {
    case ComparisonNodeType::EQ:
        return *leftVal == *rightVal;
    case ComparisonNodeType::NE:
        return *leftVal != *rightVal;
    case ComparisonNodeType::LT:
        return *leftVal < *rightVal;
    case ComparisonNodeType::LE:
        return *leftVal <= *rightVal;
    case ComparisonNodeType::GT:
        return *leftVal > *rightVal;
    case ComparisonNodeType::GE:
        return *leftVal >= *rightVal;
    }
}

EvalNodeType
ComparisonNode::getType() const
{
    return EvalNodeType::COMPARISON_OP;
}

}