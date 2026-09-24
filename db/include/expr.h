#ifndef EXPR_H
#define EXPR_H

#include "common.h"
#include "schema.h"
#include "row.h"

typedef enum ExprType {
    EXPR_COMPARISON,
    EXPR_LOGICAL
} ExprType;

typedef enum LogicalOp {
    LOGICAL_AND,
    LOGICAL_OR
} LogicalOp;

typedef struct Literal {
    bool is_string;
    uint32_t int_value;
    char str_value[MAX_STR_LEN];
} Literal;

typedef struct Expr {
    ExprType type;

    /* EXPR_COMPARISON fields */
    char column[MAX_NAME_LEN];
    WhereOp op;
    Literal value;

    /* EXPR_LOGICAL fields */
    LogicalOp log_op;
    struct Expr* left;
    struct Expr* right;
} Expr;

void free_expr(Expr* expr);
bool evaluate_expr(const Expr* expr, const DynamicRow* row, const Schema* schema);

#endif /* EXPR_H */
