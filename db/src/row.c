#include "row.h"

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
