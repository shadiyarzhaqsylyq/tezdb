#ifndef DB_SCHEMA_H
#define DB_SCHEMA_H

#include "common.h"

void schema_add_column(Schema* schema, const char* name, DataType type,
                       uint32_t length, bool is_pk);

int strcasecmp_custom(const char* s1, const char* s2);

int schema_find_column(const Schema* schema, const char* name);

void build_combined_schema(Schema* out, const Schema* left, const Schema* right);

uint32_t get_pk_value(const DynamicRow* row, const Schema* schema);

void serialize_row(const DynamicRow* source, void* destination, const Schema* schema);
void deserialize_row(const void* source, DynamicRow* destination, const Schema* schema);
void print_row(const DynamicRow* row, const Schema* schema);

void free_expr(Expr* expr);
WhereOp flip_op(WhereOp op);
bool evaluate_expr(const Expr* expr, const DynamicRow* row, const Schema* schema);

#endif /* DB_SCHEMA_H */
