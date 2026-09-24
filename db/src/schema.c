#include "schema.h"

void schema_add_column(Schema* schema, const char* name, DataType type,
                       uint32_t length, bool is_pk) {
    if (schema->num_columns >= MAX_COLUMNS) return;

    ColumnDef* col = &schema->columns[schema->num_columns];
    strncpy(col->name, name, MAX_NAME_LEN - 1);
    col->name[MAX_NAME_LEN - 1] = '\0';
    col->type = type;
    col->is_primary_key = is_pk;
    col->offset = schema->row_size;

    if (type == DATA_TYPE_INT) {
        col->length = 0;
        col->size = sizeof(int32_t);
    } else {
        col->length = (length > 0) ? length : 32;
        col->size = col->length + 1;
    }

    if (is_pk) {
        schema->primary_key_index = schema->num_columns;
    }

    schema->row_size += col->size;
    schema->num_columns++;
    schema->has_schema = true;
}

int strcasecmp_custom(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int schema_find_column(const Schema* schema, const char* name) {
    for (uint32_t i = 0; i < schema->num_columns; ++i) {
        if (strcasecmp_custom(schema->columns[i].name, name) == 0) {
            return (int)i;
        }
    }

    const char* dot = strrchr(name, '.');
    if (dot) {
        for (uint32_t i = 0; i < schema->num_columns; ++i) {
            if (strcasecmp_custom(schema->columns[i].name, dot + 1) == 0) {
                return (int)i;
            }
        }
        return -1;
    }

    int found = -1;
    for (uint32_t i = 0; i < schema->num_columns; ++i) {
        const char* col_dot = strrchr(schema->columns[i].name, '.');
        if (!col_dot) continue;
        if (strcasecmp_custom(col_dot + 1, name) == 0) {
            if (found >= 0) return -1;
            found = (int)i;
        }
    }
    return found;
}

void build_combined_schema(Schema* out, const Schema* left, const Schema* right) {
    char tmp[MAX_NAME_LEN * 2];

    memset(out, 0, sizeof(Schema));
    out->has_schema = true;

    snprintf(tmp, sizeof(tmp), "%s+%s", left->table_name, right->table_name);
    strncpy(out->table_name, tmp, MAX_NAME_LEN - 1);

    uint32_t n = 0;
    for (uint32_t i = 0; i < left->num_columns && n < MAX_COLUMNS; i++) {
        out->columns[n] = left->columns[i];
        snprintf(tmp, sizeof(tmp), "%s.%s", left->table_name, left->columns[i].name);
        strncpy(out->columns[n].name, tmp, MAX_NAME_LEN - 1);
        n++;
    }
    for (uint32_t i = 0; i < right->num_columns && n < MAX_COLUMNS; i++) {
        out->columns[n] = right->columns[i];
        snprintf(tmp, sizeof(tmp), "%s.%s", right->table_name, right->columns[i].name);
        strncpy(out->columns[n].name, tmp, MAX_NAME_LEN - 1);
        n++;
    }
    out->num_columns = n;
}

uint32_t get_pk_value(const DynamicRow* row, const Schema* schema) {
    if (schema->primary_key_index < row->num_values) {
        return (uint32_t)row->values[schema->primary_key_index].int_val;
    }
    return 0;
}

void serialize_row(const DynamicRow* source, void* destination, const Schema* schema) {
    memset(destination, 0, schema->row_size);
    for (uint32_t i = 0; i < schema->num_columns; ++i) {
        const ColumnDef* col = &schema->columns[i];
        uint8_t* dest_ptr = (uint8_t*)destination + col->offset;
        if (i < source->num_values) {
            const Value* val = &source->values[i];
            if (col->type == DATA_TYPE_INT) {
                int32_t v = val->int_val;
                memcpy(dest_ptr, &v, sizeof(int32_t));
            } else {
                strncpy((char*)dest_ptr, val->str_val, col->length);
                dest_ptr[col->length] = '\0';
            }
        }
    }
}

void deserialize_row(const void* source, DynamicRow* destination, const Schema* schema) {
    destination->num_values = schema->num_columns;
    for (uint32_t i = 0; i < schema->num_columns; ++i) {
        const ColumnDef* col = &schema->columns[i];
        const uint8_t* src_ptr = (const uint8_t*)source + col->offset;
        if (col->type == DATA_TYPE_INT) {
            int32_t v = 0;
            memcpy(&v, src_ptr, sizeof(int32_t));
            destination->values[i].type = DATA_TYPE_INT;
            destination->values[i].int_val = v;
        } else {
            destination->values[i].type = DATA_TYPE_VARCHAR;
            memcpy(destination->values[i].str_val, src_ptr, col->size);
            destination->values[i].str_val[col->size - 1] = '\0';
        }
    }
}

void print_row(const DynamicRow* row, const Schema* schema) {
    printf("(");
    for (uint32_t i = 0; i < schema->num_columns; ++i) {
        if (i > 0) printf(", ");
        if (schema->columns[i].type == DATA_TYPE_INT) {
            printf("%d", row->values[i].int_val);
        } else {
            printf("'%s'", row->values[i].str_val);
        }
    }
    printf(")\n");
}

void free_expr(Expr* expr) {
    if (!expr) return;
    if (expr->type == EXPR_LOGICAL) {
        free_expr(expr->left);
        free_expr(expr->right);
    }
    free(expr);
}

WhereOp flip_op(WhereOp op) {
    switch (op) {
        case WHERE_OP_GT: return WHERE_OP_LT;
        case WHERE_OP_LT: return WHERE_OP_GT;
        case WHERE_OP_GE: return WHERE_OP_LE;
        case WHERE_OP_LE: return WHERE_OP_GE;
        default: return op;
    }
}

bool evaluate_expr(const Expr* expr, const DynamicRow* row, const Schema* schema) {
    if (!expr) return true;

    if (expr->type == EXPR_LOGICAL) {
        if (expr->log_op == LOGICAL_AND) {
            return evaluate_expr(expr->left, row, schema) &&
                   evaluate_expr(expr->right, row, schema);
        } else if (expr->log_op == LOGICAL_OR) {
            return evaluate_expr(expr->left, row, schema) ||
                   evaluate_expr(expr->right, row, schema);
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
