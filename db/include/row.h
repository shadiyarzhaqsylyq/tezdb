#ifndef ROW_H
#define ROW_H

#include "common.h"
#include "schema.h"

typedef struct Value {
    DataType type;
    int32_t int_val;
    char str_val[MAX_STR_LEN];
} Value;

typedef struct DynamicRow {
    Value values[MAX_COLUMNS];
    uint32_t num_values;
} DynamicRow;

uint32_t get_pk_value(const DynamicRow* row, const Schema* schema);
void serialize_row(const DynamicRow* source, void* destination, const Schema* schema);
void deserialize_row(const void* source, DynamicRow* destination, const Schema* schema);
void print_row(const DynamicRow* row, const Schema* schema);

#endif /* ROW_H */
