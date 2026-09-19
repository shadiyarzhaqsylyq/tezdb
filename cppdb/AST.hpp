#pragma once

#include "Types.hpp"
#include <string>
#include <memory>
#include <utility>

enum class WhereOp {
    Eq,
    Neq,
    Gt,
    Lt,
    Ge,
    Le
};

enum class LogicalOp {
    And,
    Or
};

class Expr {
public:
    virtual ~Expr() = default;
    [[nodiscard]] virtual bool evaluate(const DynamicRow& row, const Schema& schema) const = 0;
};

class ComparisonExpr : public Expr {
public:
    ComparisonExpr(std::string column, WhereOp op, Value value)
        : column_(std::move(column)), op_(op), value_(std::move(value)) {}

    [[nodiscard]] bool evaluate(const DynamicRow& row, const Schema& schema) const override {
        int col_idx = schema.find_column(column_);
        if (col_idx < 0 || static_cast<size_t>(col_idx) >= row.values.size()) return false;

        const auto& cell = row.values[col_idx];
        const auto& col_def = schema.columns[col_idx];

        int cmp = 0;
        if (col_def.type == DataType::Int) {
            int32_t v1 = std::holds_alternative<int32_t>(cell) ? std::get<int32_t>(cell) : 0;
            int32_t v2 = std::holds_alternative<int32_t>(value_) ? std::get<int32_t>(value_) : 0;
            cmp = (v1 > v2) - (v1 < v2);
        } else {
            std::string s1 = std::holds_alternative<std::string>(cell) ? std::get<std::string>(cell) : "";
            std::string s2 = std::holds_alternative<std::string>(value_) ? std::get<std::string>(value_) : "";
            cmp = (s1 > s2) - (s1 < s2);
        }

        switch (op_) {
            case WhereOp::Eq:  return cmp == 0;
            case WhereOp::Neq: return cmp != 0;
            case WhereOp::Gt:  return cmp > 0;
            case WhereOp::Lt:  return cmp < 0;
            case WhereOp::Ge:  return cmp >= 0;
            case WhereOp::Le:  return cmp <= 0;
        }
        return false;
    }

private:
    std::string column_;
    WhereOp op_;
    Value value_;
};

class LogicalExpr : public Expr {
public:
    LogicalExpr(LogicalOp op, std::unique_ptr<Expr> left, std::unique_ptr<Expr> right)
        : op_(op), left_(std::move(left)), right_(std::move(right)) {}

    [[nodiscard]] bool evaluate(const DynamicRow& row, const Schema& schema) const override {
        if (op_ == LogicalOp::And) {
            return left_->evaluate(row, schema) && right_->evaluate(row, schema);
        } else {
            return left_->evaluate(row, schema) || right_->evaluate(row, schema);
        }
    }

private:
    LogicalOp op_;
    std::unique_ptr<Expr> left_;
    std::unique_ptr<Expr> right_;
};
