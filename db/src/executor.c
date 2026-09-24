#include "executor.h"

bool row_matches_where(const DynamicRow* row, const Statement* statement, const Schema* schema) {
    if (!statement->where) return true;
    return evaluate_expr(statement->where, row, schema);
}

ExecuteResult execute_insert(Statement* statement, Table* table) {
    if (table->pager->num_pages + INSERT_PAGE_SAFETY_MARGIN > TABLE_MAX_PAGES) {
        return EXECUTE_TABLE_FULL;
    }

    uint32_t key_to_insert = get_pk_value(&statement->row_to_insert, &table->schema);
    Cursor* cursor = table_find(table, key_to_insert);

    void* node = pager_get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    if (cursor->cell_num < num_cells) {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num, table->schema.row_size);
        if (key_at_index == key_to_insert) {
            free(cursor);
            return EXECUTE_DUPLICATE_KEY;
        }
    }

    leaf_node_insert(cursor, key_to_insert, &statement->row_to_insert);
    free(cursor);
    return EXECUTE_SUCCESS;
}

typedef struct PendingUpdate {
    uint32_t old_key;
    uint32_t new_key;
    DynamicRow row;
} PendingUpdate;

ExecuteResult execute_update(Statement* statement, Table* table) {
    uint32_t capacity = 16;
    PendingUpdate* pending = (PendingUpdate*)malloc(capacity * sizeof(PendingUpdate));
    uint32_t pending_count = 0;

    Cursor* cursor = table_start(table);
    DynamicRow row;

    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &row, &table->schema);
        if (row_matches_where(&row, statement, &table->schema)) {
            uint32_t old_key = get_pk_value(&row, &table->schema);

            for (uint32_t i = 0; i < statement->num_update_assignments; i++) {
                UpdateAssignment* assign = &statement->update_assignments[i];
                int col_idx = schema_find_column(&table->schema, assign->column_name);
                if (col_idx >= 0 && (uint32_t)col_idx < row.num_values) {
                    if (table->schema.columns[col_idx].type == DATA_TYPE_INT) {
                        row.values[col_idx].int_val = (int32_t)strtol(assign->value_text, NULL, 10);
                    } else {
                        strncpy(row.values[col_idx].str_val, assign->value_text, MAX_STR_LEN - 1);
                    }
                }
            }

            if (pending_count == capacity) {
                capacity *= 2;
                pending = (PendingUpdate*)realloc(pending, capacity * sizeof(PendingUpdate));
            }
            pending[pending_count].old_key = old_key;
            pending[pending_count].new_key = get_pk_value(&row, &table->schema);
            pending[pending_count].row = row;
            pending_count++;
        }
        cursor_advance(cursor);
    }
    free(cursor);

    uint32_t applied = 0;
    uint32_t skipped_duplicates = 0;

    for (uint32_t i = 0; i < pending_count; i++) {
        PendingUpdate* u = &pending[i];

        if (u->new_key == u->old_key) {
            Cursor* c = table_find(table, u->old_key);
            void* node = pager_get_page(table->pager, c->page_num);
            uint32_t num_cells = *leaf_node_num_cells(node);
            if (c->cell_num < num_cells &&
                *leaf_node_key(node, c->cell_num, table->schema.row_size) == u->old_key) {
                serialize_row(&u->row, cursor_value(c), &table->schema);
                applied++;
            }
            free(c);
            continue;
        }

        Cursor* dest = table_find(table, u->new_key);
        void* dest_node = pager_get_page(table->pager, dest->page_num);
        uint32_t dest_num_cells = *leaf_node_num_cells(dest_node);
        bool duplicate = (dest->cell_num < dest_num_cells &&
                           *leaf_node_key(dest_node, dest->cell_num, table->schema.row_size) == u->new_key);
        free(dest);

        if (duplicate) {
            skipped_duplicates++;
            continue;
        }

        Cursor* old_cursor = table_find(table, u->old_key);
        void* old_node = pager_get_page(table->pager, old_cursor->page_num);
        uint32_t old_num_cells = *leaf_node_num_cells(old_node);
        if (old_cursor->cell_num < old_num_cells &&
            *leaf_node_key(old_node, old_cursor->cell_num, table->schema.row_size) == u->old_key) {
            leaf_node_delete(old_cursor);
            free(old_cursor);

            Cursor* insert_cursor = table_find(table, u->new_key);
            leaf_node_insert(insert_cursor, u->new_key, &u->row);
            free(insert_cursor);
            applied++;
        } else {
            free(old_cursor);
        }
    }

    free(pending);

    if (skipped_duplicates > 0) {
        printf("UPDATE %u (skipped %u due to duplicate key)\n", applied, skipped_duplicates);
    } else {
        printf("UPDATE %u\n", applied);
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_delete(Statement* statement, Table* table) {
    static uint32_t keys_to_delete[PAGE_SIZE];
    uint32_t delete_count = 0;

    Cursor* cursor = table_start(table);
    DynamicRow row;

    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &row, &table->schema);
        if (row_matches_where(&row, statement, &table->schema)) {
            if (delete_count < PAGE_SIZE) {
                keys_to_delete[delete_count++] = get_pk_value(&row, &table->schema);
            }
        }
        cursor_advance(cursor);
    }
    free(cursor);

    for (uint32_t i = 0; i < delete_count; i++) {
        uint32_t key = keys_to_delete[i];
        Cursor* c = table_find(table, key);
        void* node = pager_get_page(table->pager, c->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);
        if (c->cell_num < num_cells && *leaf_node_key(node, c->cell_num, table->schema.row_size) == key) {
            leaf_node_delete(c);
        }
        free(c);
    }
    printf("DELETE %u\n", delete_count);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table) {
    Cursor* cursor = table_start(table);
    DynamicRow row;
    uint32_t match_count = 0;

    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &row, &table->schema);
        if (row_matches_where(&row, statement, &table->schema)) {
            match_count++;
            if (!statement->is_count) print_row(&row, &table->schema);
        }
        cursor_advance(cursor);
    }
    free(cursor);

    if (statement->is_count) {
        printf("%u row(s).\n", match_count);
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_begin(Table* table) {
    if (table->in_transaction) return EXECUTE_TX_ALREADY_ACTIVE;

    table->in_transaction = true;
    table->tx_original_num_pages = table->pager->num_pages;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        void* page_ptr = table->pager->pages[i];
        if (page_ptr != NULL) {
            table->tx_backup[i] = malloc(PAGE_SIZE);
            memcpy(table->tx_backup[i], page_ptr, PAGE_SIZE);
            table->tx_was_cached[i] = true;
        } else {
            table->tx_backup[i] = NULL;
            table->tx_was_cached[i] = false;
        }
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_commit(Table* table) {
    if (!table->in_transaction) return EXECUTE_NO_ACTIVE_TX;

    tx_free_backups(table);
    for (uint32_t i = 0; i < table->pager->num_pages; i++) {
        if (table->pager->pages[i] != NULL) {
            pager_flush(table->pager, i);
        }
    }
    table->in_transaction = false;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_rollback(Table* table) {
    if (!table->in_transaction) return EXECUTE_NO_ACTIVE_TX;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (i < table->tx_original_num_pages) {
            if (table->tx_was_cached[i]) {
                memcpy(table->pager->pages[i], table->tx_backup[i], PAGE_SIZE);
                free(table->tx_backup[i]);
                table->tx_backup[i] = NULL;
            } else if (table->pager->pages[i] != NULL) {
                free(table->pager->pages[i]);
                table->pager->pages[i] = NULL;
            }
        } else {
            if (table->pager->pages[i] != NULL) {
                free(table->pager->pages[i]);
                table->pager->pages[i] = NULL;
            }
            if (table->tx_backup[i] != NULL) {
                free(table->tx_backup[i]);
                table->tx_backup[i] = NULL;
            }
        }
    }
    table->pager->num_pages = table->tx_original_num_pages;
    table->in_transaction = false;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* statement, Table* table) {
    switch (statement->type) {
        case STATEMENT_INSERT:
            return execute_insert(statement, table);
        case STATEMENT_SELECT:
            return execute_select(statement, table);
        case STATEMENT_UPDATE:
            return execute_update(statement, table);
        case STATEMENT_DELETE:
            return execute_delete(statement, table);
        case STATEMENT_CREATE: {
            table->schema = statement->created_schema;

            MetaPage* meta = (MetaPage*)pager_get_page(table->pager, 0);
            meta->magic = SCHEMA_MAGIC;
            meta->root_page_num = table->root_page_num;
            meta->schema = table->schema;

            void* root_node = pager_get_page(table->pager, table->root_page_num);
            initialize_leaf_node(root_node);
            set_node_root(root_node, true);
            printf("CREATE TABLE (%u columns configured)\n", table->schema.num_columns);
            return EXECUTE_SUCCESS;
        }
        case STATEMENT_BEGIN:
            return execute_begin(table);
        case STATEMENT_COMMIT:
            return execute_commit(table);
        case STATEMENT_ROLLBACK:
            return execute_rollback(table);
    }
    return EXECUTE_SUCCESS;
}
