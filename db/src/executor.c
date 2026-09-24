#include "executor.h"



typedef struct PkRange {
    bool is_constrained;     // True if we can narrow the scan
    bool has_point_lookup;   // True if we found: pk = <val>
    uint32_t point_key;      // The exact key to find
    uint32_t min_key;        // Start scan here (default 0)
    uint32_t max_key;        // Stop scan when key > max_key (default UINT32_MAX)
    bool is_impossible;      // e.g. id > 10 AND id < 5 (no rows can match)
} PkRange;

static void analyze_pk_expr(const Expr* expr, const char* pk_col_name, PkRange* range) {
    if (!expr || range->is_impossible) return;

    if (expr->type == EXPR_LOGICAL) {
        // If OR is present, safe bounding is complex; fall back to full table scan
        if (expr->log_op == LOGICAL_OR) {
            range->is_constrained = false;
            range->has_point_lookup = false;
            return;
        }
        // For AND, recursively intersect bounds
        analyze_pk_expr(expr->left, pk_col_name, range);
        analyze_pk_expr(expr->right, pk_col_name, range);
        return;
    }

    // Leaf comparison node: check if it targets the primary key column
    if (strcasecmp_custom(expr->column, pk_col_name) != 0) return;
    if (expr->value.is_string) return; // PK is INT

    uint32_t val = expr->value.int_value;
    range->is_constrained = true;

    switch (expr->op) {
        case WHERE_OP_EQ:
            if (range->has_point_lookup && range->point_key != val) {
                range->is_impossible = true; // id = 3 AND id = 5
            }
            range->has_point_lookup = true;
            range->point_key = val;
            if (val < range->min_key || val > range->max_key) {
                range->is_impossible = true;
            }
            range->min_key = val;
            range->max_key = val;
            break;

        case WHERE_OP_GE:
            if (val > range->min_key) range->min_key = val;
            break;

        case WHERE_OP_GT:
            if (val == UINT32_MAX) range->is_impossible = true;
            else if (val + 1 > range->min_key) range->min_key = val + 1;
            break;

        case WHERE_OP_LE:
            if (val < range->max_key) range->max_key = val;
            break;

        case WHERE_OP_LT:
            if (val == 0) range->is_impossible = true;
            else if (val - 1 < range->max_key) range->max_key = val - 1;
            break;

        case WHERE_OP_NEQ:
            // Handled by row_matches_where
            break;
    }

    if (range->min_key > range->max_key) {
        range->is_impossible = true;
    }
}

static PkRange get_pk_scan_range(const Statement* statement, const Schema* schema) {
    PkRange range = {
        .is_constrained = false,
        .has_point_lookup = false,
        .point_key = 0,
        .min_key = 0,
        .max_key = UINT32_MAX,
        .is_impossible = false
    };

    if (!statement->where) return range;

    const char* pk_col = schema->columns[schema->primary_key_index].name;
    analyze_pk_expr(statement->where, pk_col, &range);
    return range;
}





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

// executor.c

ExecuteResult execute_delete(Statement* statement, Table* table) {
    static uint32_t keys_to_delete[PAGE_SIZE];
    uint32_t delete_count = 0;
    DynamicRow row;

    PkRange range = get_pk_scan_range(statement, &table->schema);

    if (!range.is_impossible) {
        if (range.has_point_lookup) {
            Cursor* cursor = table_find(table, range.point_key);
            cursor_normalize(cursor);
            if (!cursor->end_of_table) {
                void* node = pager_get_page(table->pager, cursor->page_num);
                uint32_t num_cells = *leaf_node_num_cells(node);
                if (cursor->cell_num < num_cells &&
                    *leaf_node_key(node, cursor->cell_num, table->schema.row_size) == range.point_key) {
                    deserialize_row(cursor_value(cursor), &row, &table->schema);
                    if (row_matches_where(&row, statement, &table->schema)) {
                        keys_to_delete[delete_count++] = range.point_key;
                    }
                }
            }
            free(cursor);
        } else {
            Cursor* cursor = range.is_constrained ? table_find(table, range.min_key)
                                                  : table_start(table);
            cursor_normalize(cursor);
            while (!cursor->end_of_table) {
                void* node = pager_get_page(table->pager, cursor->page_num);
                uint32_t cur_key = *leaf_node_key(node, cursor->cell_num, table->schema.row_size);
                if (range.is_constrained && cur_key > range.max_key) break;

                deserialize_row(cursor_value(cursor), &row, &table->schema);
                if (row_matches_where(&row, statement, &table->schema)) {
                    if (delete_count < PAGE_SIZE) {
                        keys_to_delete[delete_count++] = cur_key;
                    }
                }
                cursor_advance(cursor);
            }
            free(cursor);
        }
    }

    // Perform deletions
    for (uint32_t i = 0; i < delete_count; i++) {
        uint32_t key = keys_to_delete[i];
        Cursor* c = table_find(table, key);
        void* node = pager_get_page(table->pager, c->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);
        if (c->cell_num < num_cells &&
            *leaf_node_key(node, c->cell_num, table->schema.row_size) == key) {
            leaf_node_delete(c);
        }
        free(c);
    }

    printf("DELETE %u\n", delete_count);
    return EXECUTE_SUCCESS;
}

static void sort_rows(DynamicRow* rows, uint32_t n, uint32_t col_idx, DataType type, bool desc) {
    for (uint32_t i = 1; i < n; i++) {
        DynamicRow key = rows[i];
        long j = (long)i - 1;
        while (j >= 0) {
            int cmp;
            if (type == DATA_TYPE_INT) {
                int32_t a = rows[j].values[col_idx].int_val;
                int32_t b = key.values[col_idx].int_val;
                cmp = (a > b) - (a < b);
            } else {
                cmp = strcmp(rows[j].values[col_idx].str_val, key.values[col_idx].str_val);
            }
            bool should_shift = desc ? (cmp < 0) : (cmp > 0);
            if (!should_shift) break;
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = key;
    }
}

static void print_aggregate(const Statement* statement, const DynamicRow* rows,
                            uint32_t count, const Schema* schema) {
    if (statement->agg_type == AGG_COUNT) {
        printf("%u row(s).\n", count);
        return;
    }

    const char* agg_name = statement->agg_type == AGG_SUM ? "SUM" :
                           statement->agg_type == AGG_AVG ? "AVG" :
                           statement->agg_type == AGG_MIN ? "MIN" : "MAX";

    int col_idx = schema_find_column(schema, statement->agg_column);
    if (col_idx < 0 || schema->columns[col_idx].type != DATA_TYPE_INT) {
        printf("Error: %s can only be used on an INT column.\n", agg_name);
        return;
    }
    if (count == 0) {
        printf("(no rows)\n");
        return;
    }

    long long sum = 0;
    int32_t mn = rows[0].values[col_idx].int_val;
    int32_t mx = rows[0].values[col_idx].int_val;
    for (uint32_t i = 0; i < count; i++) {
        int32_t v = rows[i].values[col_idx].int_val;
        sum += v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }

    switch (statement->agg_type) {
        case AGG_SUM: printf("%lld\n", sum); break;
        case AGG_AVG: printf("%.4f\n", (double)sum / (double)count); break;
        case AGG_MIN: printf("%d\n", mn); break;
        case AGG_MAX: printf("%d\n", mx); break;
        default: break;
    }
}

void print_projected_row(const DynamicRow* row, const Schema* schema, const Statement* statement) {
    if (statement->select_all) {
        print_row(row, schema);
        return;
    }

    printf("(");
    for (uint32_t i = 0; i < statement->num_select_columns; ++i) {
        if (i > 0) printf(", ");
        uint32_t col_idx = statement->select_column_indices[i];
        if (schema->columns[col_idx].type == DATA_TYPE_INT) {
            printf("%d", row->values[col_idx].int_val);
        } else {
            printf("'%s'", row->values[col_idx].str_val);
        }
    }
    printf(")\n");
}

static ExecuteResult finish_select(Statement* statement, DynamicRow* rows,
                                   uint32_t count, const Schema* schema) {
    if (statement->agg_type != AGG_NONE) {
        print_aggregate(statement, rows, count, schema);
        return EXECUTE_SUCCESS;
    }

    if (statement->has_order_by) {
        int col_idx = schema_find_column(schema, statement->order_by_column);
        if (col_idx >= 0) {
            sort_rows(rows, count, (uint32_t)col_idx,
                      schema->columns[col_idx].type, statement->order_by_desc);
        }
    }

    uint32_t start = statement->has_offset ? statement->offset_count : 0;
    if (start > count) start = count;
    uint64_t end64 = statement->has_limit
                         ? (uint64_t)start + (uint64_t)statement->limit_count
                         : (uint64_t)count;
    uint32_t end = (end64 > count) ? count : (uint32_t)end64;

    for (uint32_t i = start; i < end; i++) {
        print_projected_row(&rows[i], schema, statement);
    }
    return EXECUTE_SUCCESS;
}



ExecuteResult execute_select(Statement* statement, Table* table) {
    uint32_t capacity = 64;
    uint32_t count = 0;
    DynamicRow* rows = (DynamicRow*)malloc(capacity * sizeof(DynamicRow));
    DynamicRow row;

    PkRange range = get_pk_scan_range(statement, &table->schema);

    // Short-circuit: condition can never match any row (e.g., id > 10 AND id < 5)
    if (range.is_impossible) {
        ExecuteResult res = finish_select(statement, rows, 0, &table->schema);
        free(rows);
        return res;
    }

    // PATH 1: Exact Point Lookup (WHERE id = X) -> O(log N)
    if (range.has_point_lookup) {
        Cursor* cursor = table_find(table, range.point_key);
        cursor_normalize(cursor);

        if (!cursor->end_of_table) {
            void* node = pager_get_page(table->pager, cursor->page_num);
            uint32_t num_cells = *leaf_node_num_cells(node);

            if (cursor->cell_num < num_cells) {
                uint32_t key = *leaf_node_key(node, cursor->cell_num, table->schema.row_size);
                if (key == range.point_key) {
                    deserialize_row(cursor_value(cursor), &row, &table->schema);
                    // Verify any secondary non-PK conditions (e.g. id = 5 AND name = 'Alice')
                    if (row_matches_where(&row, statement, &table->schema)) {
                        rows[count++] = row;
                    }
                }
            }
        }
        free(cursor);
    }
    // PATH 2: Range Scan or Full Scan -> O(log N + M)
    else {
        // Jump directly to min_key instead of scanning from page 0!
        Cursor* cursor = range.is_constrained ? table_find(table, range.min_key)
                                              : table_start(table);
        cursor_normalize(cursor);

        while (!cursor->end_of_table) {
            void* node = pager_get_page(table->pager, cursor->page_num);
            uint32_t cur_key = *leaf_node_key(node, cursor->cell_num, table->schema.row_size);

            // B+Tree leaf keys are sorted: stop scanning once past max_key
            if (range.is_constrained && cur_key > range.max_key) {
                break;
            }

            deserialize_row(cursor_value(cursor), &row, &table->schema);
            if (row_matches_where(&row, statement, &table->schema)) {
                if (count == capacity) {
                    capacity *= 2;
                    rows = (DynamicRow*)realloc(rows, capacity * sizeof(DynamicRow));
                }
                rows[count++] = row;
            }

            cursor_advance(cursor);
        }
        free(cursor);
    }

    ExecuteResult result = finish_select(statement, rows, count, &table->schema);
    free(rows);
    return result;
}

static bool join_values_match(const Value* a, const Value* b, DataType type, WhereOp op) {
    int cmp;
    if (type == DATA_TYPE_INT) {
        cmp = (a->int_val > b->int_val) - (a->int_val < b->int_val);
    } else {
        int c = strcmp(a->str_val, b->str_val);
        cmp = (c > 0) - (c < 0);
    }
    switch (op) {
        case WHERE_OP_EQ:  return cmp == 0;
        case WHERE_OP_NEQ: return cmp != 0;
        case WHERE_OP_GT:  return cmp > 0;
        case WHERE_OP_LT:  return cmp < 0;
        case WHERE_OP_GE:  return cmp >= 0;
        case WHERE_OP_LE:  return cmp <= 0;
    }
    return false;
}

ExecuteResult execute_select_join(Statement* statement, Table* left, Table* right) {
    if (left->schema.num_columns + right->schema.num_columns > MAX_COLUMNS) {
        printf("Error: joined tables have too many combined columns (max %d).\n", MAX_COLUMNS);
        return EXECUTE_TOO_MANY_COLUMNS;
    }

    int left_col_idx = schema_find_column(&left->schema, statement->join_left_column);
    int right_col_idx = schema_find_column(&right->schema, statement->join_right_column);
    if (left_col_idx < 0 || right_col_idx < 0) {
        printf("Error: join column not found.\n");
        return EXECUTE_COLUMN_NOT_FOUND;
    }
    DataType join_type = left->schema.columns[left_col_idx].type;

    Schema combined;
    build_combined_schema(&combined, &left->schema, &right->schema);

    uint32_t right_cap = 64, right_count = 0;
    DynamicRow* right_rows = (DynamicRow*)malloc(right_cap * sizeof(DynamicRow));
    Cursor* rc = table_start(right);
    DynamicRow rrow;
    while (!rc->end_of_table) {
        deserialize_row(cursor_value(rc), &rrow, &right->schema);
        if (right_count == right_cap) {
            right_cap *= 2;
            right_rows = (DynamicRow*)realloc(right_rows, right_cap * sizeof(DynamicRow));
        }
        right_rows[right_count++] = rrow;
        cursor_advance(rc);
    }
    free(rc);

    uint32_t cap = 64, count = 0;
    DynamicRow* rows = (DynamicRow*)malloc(cap * sizeof(DynamicRow));

    Cursor* lc = table_start(left);
    DynamicRow lrow;
    while (!lc->end_of_table) {
        deserialize_row(cursor_value(lc), &lrow, &left->schema);
        for (uint32_t j = 0; j < right_count; j++) {
            if (!join_values_match(&lrow.values[left_col_idx],
                                   &right_rows[j].values[right_col_idx],
                                   join_type, statement->join_op)) {
                continue;
            }

            DynamicRow combined_row;
            uint32_t w = 0;
            for (uint32_t k = 0; k < lrow.num_values && w < MAX_COLUMNS; k++)
                combined_row.values[w++] = lrow.values[k];
            for (uint32_t k = 0; k < right_rows[j].num_values && w < MAX_COLUMNS; k++)
                combined_row.values[w++] = right_rows[j].values[k];
            combined_row.num_values = w;

            if (!row_matches_where(&combined_row, statement, &combined)) continue;

            if (count == cap) {
                cap *= 2;
                rows = (DynamicRow*)realloc(rows, cap * sizeof(DynamicRow));
            }
            rows[count++] = combined_row;
        }
        cursor_advance(lc);
    }
    free(lc);
    free(right_rows);

    ExecuteResult result = finish_select(statement, rows, count, &combined);
    free(rows);
    return result;
}

ExecuteResult execute_begin(Database* db) {
    if (db->in_transaction) return EXECUTE_TX_ALREADY_ACTIVE;

    db->in_transaction = true;
    db->tx_original_num_pages = db->pager->num_pages;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        void* page_ptr = db->pager->pages[i];
        if (page_ptr != NULL) {
            db->tx_backup[i] = malloc(PAGE_SIZE);
            memcpy(db->tx_backup[i], page_ptr, PAGE_SIZE);
            db->tx_was_cached[i] = true;
        } else {
            db->tx_backup[i] = NULL;
            db->tx_was_cached[i] = false;
        }
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_commit(Database* db) {
    if (!db->in_transaction) return EXECUTE_NO_ACTIVE_TX;

    tx_free_backups(db);
    for (uint32_t i = 0; i < db->pager->num_pages; i++) {
        if (db->pager->pages[i] != NULL) {
            pager_flush(db->pager, i);
        }
    }
    db->in_transaction = false;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_rollback(Database* db) {
    if (!db->in_transaction) return EXECUTE_NO_ACTIVE_TX;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (i < db->tx_original_num_pages) {
            if (db->tx_was_cached[i]) {
                memcpy(db->pager->pages[i], db->tx_backup[i], PAGE_SIZE);
                free(db->tx_backup[i]);
                db->tx_backup[i] = NULL;
            } else if (db->pager->pages[i] != NULL) {
                free(db->pager->pages[i]);
                db->pager->pages[i] = NULL;
            }
        } else {
            if (db->pager->pages[i] != NULL) {
                free(db->pager->pages[i]);
                db->pager->pages[i] = NULL;
            }
            if (db->tx_backup[i] != NULL) {
                free(db->tx_backup[i]);
                db->tx_backup[i] = NULL;
            }
        }
    }
    db->pager->num_pages = db->tx_original_num_pages;
    db->in_transaction = false;

    off_t target_size = (off_t)db->tx_original_num_pages * PAGE_SIZE;
    struct stat st;
    if (fstat(db->pager->file_descriptor, &st) == -1) {
        printf("Warning: failed to stat file during rollback (errno %d).\n", errno);
    } else if (st.st_size > target_size) {
        if (ftruncate(db->pager->file_descriptor, target_size) == -1) {
            printf("Warning: failed to truncate file during rollback (errno %d).\n", errno);
        } else {
            db->pager->file_length = (uint32_t)target_size;
        }
    } else {
        db->pager->file_length = (uint32_t)st.st_size;
    }

    reload_catalog_from_disk(db);
    reload_free_list_from_disk(db);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_create(Statement* statement, Database* db) {
    if (database_find_table(db, statement->table_name)) {
        printf("Error: table '%s' already exists.\n", statement->table_name);
        return EXECUTE_TABLE_ALREADY_EXISTS;
    }
    if (db->num_tables >= MAX_TABLES) {
        printf("Error: catalog is full (max %d tables).\n", MAX_TABLES);
        return EXECUTE_TOO_MANY_TABLES;
    }

    int slot = -1;
    for (uint32_t i = 0; i < MAX_TABLES; i++) {
        if (!db->tables[i].in_use) {
            slot = (int)i;
            break;
        }
    }

    TableCatalogEntry* entry = &db->tables[slot];
    entry->in_use = true;
    entry->schema = statement->created_schema;
    entry->root_page_num = allocate_page(db);

    void* root_node = pager_get_page(db->pager, entry->root_page_num);
    initialize_leaf_node(root_node);
    set_node_root(root_node, true);

    db->num_tables++;
    persist_catalog_entry(db, slot);
    persist_header(db);

    printf("CREATE TABLE %s (%u columns configured)\n",
           entry->schema.table_name, entry->schema.num_columns);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_drop(Statement* statement, Database* db) {
    int idx = database_find_table_index(db, statement->table_name);
    if (idx < 0) {
        printf("Error: no such table '%s'.\n", statement->table_name);
        return EXECUTE_TABLE_NOT_FOUND;
    }

    uint32_t freed[TABLE_MAX_PAGES];
    uint32_t freed_count = 0;
    collect_table_pages(db, db->tables[idx].root_page_num, freed, &freed_count);
    for (uint32_t i = 0; i < freed_count; i++) free_page(db, freed[i]);
    persist_free_list(db);

    memset(&db->tables[idx], 0, sizeof(TableCatalogEntry));
    db->num_tables--;
    persist_catalog_entry(db, idx);
    persist_header(db);

    printf("DROP TABLE %s (%u page(s) reclaimed)\n", statement->table_name, freed_count);
    return EXECUTE_SUCCESS;
}

static ExecuteResult migrate_table_rows(Database* db, int idx, Schema new_schema, int dropped_col_idx) {
    TableCatalogEntry* entry = &db->tables[idx];

    uint32_t old_root = entry->root_page_num;
    Table old_view = {.pager = db->pager, .root_page_num = old_root,
                      .schema = entry->schema, .db = db};

    uint32_t new_root = allocate_page(db);
    void* new_root_node = pager_get_page(db->pager, new_root);
    initialize_leaf_node(new_root_node);
    set_node_root(new_root_node, true);

    Table new_view = {.pager = db->pager, .root_page_num = new_root,
                      .schema = new_schema, .db = db};

    Cursor* cursor = table_start(&old_view);
    DynamicRow old_row, new_row;

    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &old_row, &old_view.schema);

        uint32_t w = 0;
        for (uint32_t i = 0; i < old_row.num_values; i++) {
            if ((int)i == dropped_col_idx) continue;
            new_row.values[w++] = old_row.values[i];
        }
        if (dropped_col_idx < 0) {
            ColumnDef* new_col = &new_schema.columns[new_schema.num_columns - 1];
            Value* v = &new_row.values[w++];
            v->type = new_col->type;
            if (new_col->type == DATA_TYPE_INT) {
                v->int_val = 0;
            } else {
                v->str_val[0] = '\0';
            }
        }
        new_row.num_values = w;

        uint32_t key = get_pk_value(&new_row, &new_schema);
        Cursor* ins = table_find(&new_view, key);
        leaf_node_insert(ins, key, &new_row);
        free(ins);

        cursor_advance(cursor);
    }
    free(cursor);

    entry->schema = new_schema;
    entry->root_page_num = new_root;
    persist_catalog_entry(db, idx);

    uint32_t freed[TABLE_MAX_PAGES];
    uint32_t freed_count = 0;
    collect_table_pages(db, old_root, freed, &freed_count);
    for (uint32_t i = 0; i < freed_count; i++) free_page(db, freed[i]);
    persist_free_list(db);

    printf("ALTER TABLE %s: rebuilt with %u column(s) (%u page(s) reclaimed)\n",
           entry->schema.table_name, entry->schema.num_columns, freed_count);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_alter(Statement* statement, Database* db) {
    int idx = database_find_table_index(db, statement->table_name);
    if (idx < 0) {
        printf("Error: no such table '%s'.\n", statement->table_name);
        return EXECUTE_TABLE_NOT_FOUND;
    }
    TableCatalogEntry* entry = &db->tables[idx];

    switch (statement->alter_kind) {
        case ALTER_RENAME_TABLE: {
            if (database_find_table(db, statement->alter_new_name)) {
                printf("Error: table '%s' already exists.\n", statement->alter_new_name);
                return EXECUTE_TABLE_ALREADY_EXISTS;
            }
            strncpy(entry->schema.table_name, statement->alter_new_name, MAX_NAME_LEN - 1);
            entry->schema.table_name[MAX_NAME_LEN - 1] = '\0';
            persist_catalog_entry(db, idx);
            printf("ALTER TABLE: renamed '%s' to '%s'\n",
                   statement->table_name, statement->alter_new_name);
            return EXECUTE_SUCCESS;
        }
        case ALTER_RENAME_COLUMN: {
            int col_idx = schema_find_column(&entry->schema, statement->alter_column_name);
            if (col_idx < 0) {
                printf("Error: no such column '%s'.\n", statement->alter_column_name);
                return EXECUTE_COLUMN_NOT_FOUND;
            }
            if (schema_find_column(&entry->schema, statement->alter_new_name) >= 0) {
                printf("Error: column '%s' already exists.\n", statement->alter_new_name);
                return EXECUTE_COLUMN_ALREADY_EXISTS;
            }
            strncpy(entry->schema.columns[col_idx].name, statement->alter_new_name, MAX_NAME_LEN - 1);
            entry->schema.columns[col_idx].name[MAX_NAME_LEN - 1] = '\0';
            persist_catalog_entry(db, idx);
            printf("ALTER TABLE: renamed column '%s' to '%s'\n",
                   statement->alter_column_name, statement->alter_new_name);
            return EXECUTE_SUCCESS;
        }
        case ALTER_ADD_COLUMN: {
            if (schema_find_column(&entry->schema, statement->alter_column_name) >= 0) {
                printf("Error: column '%s' already exists.\n", statement->alter_column_name);
                return EXECUTE_COLUMN_ALREADY_EXISTS;
            }
            if (entry->schema.num_columns >= MAX_COLUMNS) {
                printf("Error: table already has the max %d columns.\n", MAX_COLUMNS);
                return EXECUTE_TOO_MANY_COLUMNS;
            }
            Schema new_schema = entry->schema;
            schema_add_column(&new_schema, statement->alter_column_name,
                              statement->alter_column_type, statement->alter_column_length, false);
            return migrate_table_rows(db, idx, new_schema, -1);
        }
        case ALTER_DROP_COLUMN: {
            int col_idx = schema_find_column(&entry->schema, statement->alter_column_name);
            if (col_idx < 0) {
                printf("Error: no such column '%s'.\n", statement->alter_column_name);
                return EXECUTE_COLUMN_NOT_FOUND;
            }
            if (entry->schema.columns[col_idx].is_primary_key) {
                printf("Error: cannot drop primary key column '%s'.\n", statement->alter_column_name);
                return EXECUTE_INVALID_OPERATION;
            }
            if (entry->schema.num_columns <= 1) {
                printf("Error: cannot drop the only remaining column.\n");
                return EXECUTE_INVALID_OPERATION;
            }

            Schema new_schema;
            memset(&new_schema, 0, sizeof(Schema));
            strncpy(new_schema.table_name, entry->schema.table_name, MAX_NAME_LEN - 1);
            for (uint32_t i = 0; i < entry->schema.num_columns; i++) {
                if ((int)i == col_idx) continue;
                ColumnDef* c = &entry->schema.columns[i];
                schema_add_column(&new_schema, c->name, c->type, c->length, c->is_primary_key);
            }
            return migrate_table_rows(db, idx, new_schema, col_idx);
        }
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* statement, Database* db) {
    switch (statement->type) {
        case STATEMENT_CREATE:
            return execute_create(statement, db);
        case STATEMENT_DROP:
            return execute_drop(statement, db);
        case STATEMENT_ALTER:
            return execute_alter(statement, db);
        case STATEMENT_BEGIN:
            return execute_begin(db);
        case STATEMENT_COMMIT:
            return execute_commit(db);
        case STATEMENT_ROLLBACK:
            return execute_rollback(db);
        case STATEMENT_INSERT:
        case STATEMENT_SELECT:
        case STATEMENT_UPDATE:
        case STATEMENT_DELETE: {
            const TableCatalogEntry* entry = database_find_table(db, statement->table_name);
            if (!entry) {
                printf("Error: no such table '%s'.\n", statement->table_name);
                return EXECUTE_TABLE_NOT_FOUND;
            }
            Table view = {.pager = db->pager,
                          .root_page_num = entry->root_page_num,
                          .schema = entry->schema,
                          .db = db};
            switch (statement->type) {
                case STATEMENT_INSERT:
                    return execute_insert(statement, &view);
                case STATEMENT_SELECT: {
                    if (!statement->has_join) return execute_select(statement, &view);

                    const TableCatalogEntry* right_entry =
                        database_find_table(db, statement->join_table_name);
                    if (!right_entry) {
                        printf("Error: no such table '%s'.\n", statement->join_table_name);
                        return EXECUTE_TABLE_NOT_FOUND;
                    }
                    Table right_view = {.pager = db->pager,
                                        .root_page_num = right_entry->root_page_num,
                                        .schema = right_entry->schema,
                                        .db = db};
                    return execute_select_join(statement, &view, &right_view);
                }
                case STATEMENT_UPDATE:
                    return execute_update(statement, &view);
                case STATEMENT_DELETE:
                    return execute_delete(statement, &view);
                default:
                    return EXECUTE_SUCCESS;
            }
        }
    }
    return EXECUTE_SUCCESS;
}
