#ifndef SCHEMA_H
#define SCHEMA_H

#include "common.h"

typedef struct ColumnDef {
    char name[MAX_NAME_LEN];
    DataType type;
    uint32_t length; /* For VARCHAR length; 0 for INT */
    uint32_t offset; /* Byte offset in row buffer */
    uint32_t size;   /* Size in row buffer */
    bool is_primary_key;
} ColumnDef;

typedef struct Schema {
    bool has_schema;
    char table_name[MAX_NAME_LEN];
    ColumnDef columns[MAX_COLUMNS];
    uint32_t num_columns;
    uint32_t row_size;
    uint32_t primary_key_index;
} Schema;

void schema_add_column(Schema* schema, const char* name, DataType type, uint32_t length, bool is_pk);
int schema_find_column(const Schema* schema, const char* name);

#endif /* SCHEMA_H */
