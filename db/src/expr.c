#include "expr.h"

void free_expr(Expr* expr) {
    if (!expr) return;
    if (expr->type == EXPR_LOGICAL) {
        free_expr(expr->left);
        free_expr(expr->right);
    }
    free(expr);
}

bool evaluate_expr(const Expr* expr, const DynamicRow* row, const Schema* schema) {
    if (!expr) return true;

    if (expr->type == EXPR_LOGICAL) {
        if (expr->log_op == LOGICAL_AND) {
            return evaluate_expr(expr->left, row, schema) && evaluate_expr(expr->right, row, schema);
        } else if (expr->log_op == LOGICAL_OR) {
            return evaluate_expr(expr->left, row, schema) || evaluate_expr(expr->right, row, schema);
        }
        return false;
    }

    int col_idx = schema_find_column(schema, expr->column);
    if (col_idx < 0 || (uint32_t)col_idx >= row->num_values) return false;

    const ColumnDef* col_def = &schema->columns[col_idx];
    const Value* cell_val = &row->values[col_idx];

    int cmp = 0;
    if (col_def->type == DATA_TYPE_INT) {
        int32_t v = (int32_t)expr->value.int_value;
        cmp = (cell_val->int_val > v) - (cell_val->int_val < v);
    } else {
        int c = strcmp(cell_val->str_val, expr->value.str_value);
        cmp = (c > 0) - (c < 0);
    }

    switch (expr->op) {
        case WHERE_OP_EQ:  return cmp == 0;
        case WHERE_OP_NEQ: return cmp != 0;
        case WHERE_OP_GT:  return cmp > 0;
        case WHERE_OP_LT:  return cmp < 0;
        case WHERE_OP_GE:  return cmp >= 0;
        case WHERE_OP_LE:  return cmp <= 0;
    }
    return false;
}
