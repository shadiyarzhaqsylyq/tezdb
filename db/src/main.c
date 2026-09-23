#include "common.h"
#include "database.h"
#include "parser.h"
#include "executor.h"
#include "btree.h"

void print_help(void) {
    printf("SQL Commands:\n"
           "  CREATE TABLE <name> (<pk_col> INT PRIMARY KEY, <col2> VARCHAR(32), <col3> INT, ...);\n"
           "  DROP TABLE <name>;\n"
           "  ALTER TABLE <name> ADD [COLUMN] <col> INT|VARCHAR(n);\n"
           "  ALTER TABLE <name> DROP [COLUMN] <col>;\n"
           "  ALTER TABLE <name> RENAME TO <new_name>;\n"
           "  ALTER TABLE <name> RENAME COLUMN <old> TO <new>;\n"
           "  INSERT INTO <name> VALUES (<val1>, '<val2>', ...);\n"
           "  SELECT * FROM <name> [JOIN <other> ON <col> = <col>] [WHERE <expr>] [ORDER BY <col> [ASC|DESC]] [LIMIT n] [OFFSET n];\n"
           "  SELECT COUNT(*)|SUM(col)|AVG(col)|MIN(col)|MAX(col) FROM <name> [JOIN ...] [WHERE <expr>];\n"
           "  UPDATE <name> SET <col> = <val> WHERE <expr>;\n"
           "  DELETE FROM <name> WHERE <expr>;\n"
           "  (WHERE supports =, !=, <>, <, >, <=, >=, AND, OR, and parentheses ())\n"
           "  BEGIN; | COMMIT; | ROLLBACK;\n"
           "Meta commands:\n"
           "  \\q or .exit            quit the shell\n"
           "  \\dt or .tables         list tables\n"
           "  \\d <t> or .btree <t>   print a table's B+tree structure\n"
           "  \\c <t> or .constants <t> print a table's page size constants\n"
           "  \\f or .freespace       show file size and reclaimed-page count\n"
           "  \\v or .vacuum          shrink the file by dropping trailing reclaimed pages\n"
           "  \\? or .help            show this message\n");
}

void print_tables(const Database* db) {
    if (db->num_tables == 0) {
        printf("No tables.\n");
        return;
    }
    printf("Tables:\n");
    for (uint32_t i = 0; i < MAX_TABLES; i++) {
        if (!db->tables[i].in_use) continue;
        printf("  %-32s (%u columns)\n",
               db->tables[i].schema.table_name, db->tables[i].schema.num_columns);
    }
}

void print_free_space(const Database* db) {
    printf("File: %u page(s) (%u bytes)\n",
           db->pager->num_pages, db->pager->num_pages * PAGE_SIZE);
    printf("Reclaimed pages available for reuse: %u\n", db->num_free_pages);
}

void vacuum(Database* db) {
    for (uint32_t i = 1; i < db->num_free_pages; i++) {
        uint32_t key = db->free_pages[i];
        int j = (int)i - 1;
        while (j >= 0 && db->free_pages[j] < key) {
            db->free_pages[j + 1] = db->free_pages[j];
            j--;
        }
        db->free_pages[j + 1] = key;
    }

    uint32_t reclaimed = 0;
    while (db->num_free_pages > 0 && db->free_pages[0] == db->pager->num_pages - 1) {
        uint32_t page_num = db->free_pages[0];
        for (uint32_t i = 1; i < db->num_free_pages; i++) {
            db->free_pages[i - 1] = db->free_pages[i];
        }
        db->num_free_pages--;

        if (db->pager->pages[page_num] != NULL) {
            free(db->pager->pages[page_num]);
            db->pager->pages[page_num] = NULL;
        }
        db->pager->num_pages--;
        reclaimed++;
    }

    if (reclaimed > 0) {
        off_t new_size = (off_t)db->pager->num_pages * PAGE_SIZE;
        if (ftruncate(db->pager->file_descriptor, new_size) == -1) {
            printf("Warning: VACUUM shrank the in-memory page count but the file truncate failed (errno %d).\n",
                   errno);
        } else {
            db->pager->file_length = (uint32_t)new_size;
        }
    }
    persist_free_list(db);

    printf("VACUUM: reclaimed %u trailing page(s) from the file; %u free page(s) remain available mid-file; file is now %u page(s).\n",
           reclaimed, db->num_free_pages, db->pager->num_pages);
}

void print_constants(const TableCatalogEntry* entry) {
    printf("Table: %s\n"
           "ROW_SIZE: %u\n"
           "COMMON_NODE_HEADER_SIZE: %zu\n"
           "LEAF_NODE_HEADER_SIZE: %zu\n"
           "LEAF_NODE_CELL_SIZE: %u\n"
           "LEAF_NODE_MAX_CELLS: %u\n",
           entry->schema.table_name,
           entry->schema.row_size,
           (size_t)COMMON_NODE_HEADER_SIZE,
           (size_t)LEAF_NODE_HEADER_SIZE,
           leaf_node_cell_size(entry->schema.row_size),
           leaf_node_max_cells(entry->schema.row_size));
}

static const char* meta_command_arg(const char* input) {
    const char* space = strchr(input, ' ');
    if (!space) return NULL;
    while (*space == ' ') space++;
    return (*space == '\0') ? NULL : space;
}

static const TableCatalogEntry* resolve_meta_command_table(const Database* db, const char* input) {
    const char* arg = meta_command_arg(input);
    if (arg) {
        const TableCatalogEntry* entry = database_find_table(db, arg);
        if (!entry) printf("Error: no such table '%s'.\n", arg);
        return entry;
    }
    if (db->num_tables == 1) {
        for (uint32_t i = 0; i < MAX_TABLES; i++) {
            if (db->tables[i].in_use) return &db->tables[i];
        }
    }
    printf("Usage: specify a table name, e.g. .btree <table>\n");
    return NULL;
}

MetaCommandResult do_meta_command(const char* input, Database* db) {
    if (strcmp(input, ".exit") == 0 || strcmp(input, "\\q") == 0) {
        database_close(db);
        exit(EXIT_SUCCESS);
    } else if (strcmp(input, ".tables") == 0 || strcmp(input, "\\dt") == 0) {
        print_tables(db);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input, ".vacuum") == 0 || strcmp(input, "\\v") == 0) {
        vacuum(db);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input, ".freespace") == 0 || strcmp(input, "\\f") == 0) {
        print_free_space(db);
        return META_COMMAND_SUCCESS;
    } else if (strncmp(input, ".btree", 6) == 0 || strncmp(input, "\\d", 2) == 0) {
        const TableCatalogEntry* entry = resolve_meta_command_table(db, input);
        if (entry) {
            Table view = {.pager = db->pager,
                          .root_page_num = entry->root_page_num,
                          .schema = entry->schema,
                          .db = db};
            printf("Tree (%s):\n", entry->schema.table_name);
            print_tree(&view, view.root_page_num, 0);
        }
        return META_COMMAND_SUCCESS;
    } else if (strncmp(input, ".constants", 10) == 0 || strncmp(input, "\\c", 2) == 0) {
        const TableCatalogEntry* entry = resolve_meta_command_table(db, input);
        if (entry) {
            printf("Constants:\n");
            print_constants(entry);
        }
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input, ".help") == 0 || strcmp(input, "\\?") == 0) {
        print_help();
        return META_COMMAND_SUCCESS;
    }
    return META_COMMAND_UNRECOGNIZED_COMMAND;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }

    const char* filename = argv[1];
    Database* db = database_open(filename);

    char input_buffer[1024];
    while (true) {
        printf("db=# ");
        fflush(stdout);

        if (!fgets(input_buffer, sizeof(input_buffer), stdin)) {
            break;
        }

        size_t input_len = strlen(input_buffer);
        if (input_len > 0 && input_buffer[input_len - 1] == '\n') {
            input_buffer[input_len - 1] = '\0';
        }

        if (input_buffer[0] == '\0') continue;

        if (input_buffer[0] == '.' || input_buffer[0] == '\\') {
            switch (do_meta_command(input_buffer, db)) {
                case META_COMMAND_SUCCESS:
                    continue;
                case META_COMMAND_UNRECOGNIZED_COMMAND:
                    printf("Unrecognized command '%s'\n", input_buffer);
                    continue;
            }
        }

        TokenList tokens;
        tokenize_input(input_buffer, &tokens);

        Statement statement;
        PrepareResult prep_res = prepare_statement(&tokens, db, &statement);

        switch (prep_res) {
            case PREPARE_SUCCESS:
                break;
            case PREPARE_NEGATIVE_ID:
                printf("ID must be positive.\n");
                free_expr(statement.where);
                continue;
            case PREPARE_STRING_TOO_LONG:
                printf("String is too long for column budget.\n");
                free_expr(statement.where);
                continue;
            case PREPARE_SYNTAX_ERROR:
                printf("Syntax error. Could not parse statement.\n");
                free_expr(statement.where);
                continue;
            case PREPARE_UNRECOGNIZED_STATEMENT:
                printf("Unrecognized keyword at start of '%s'.\n", input_buffer);
                free_expr(statement.where);
                continue;
            case PREPARE_NO_SUCH_TABLE:
                printf("Error: no such table '%s'.\n", statement.table_name);
                free_expr(statement.where);
                continue;
        }

        ExecuteResult exec_res = execute_statement(&statement, db);
        free_expr(statement.where);

        switch (exec_res) {
            case EXECUTE_SUCCESS:
                if (statement.type == STATEMENT_INSERT) {
                    printf("INSERT 0 1\n");
                } else if (statement.type == STATEMENT_BEGIN) {
                    printf("BEGIN\n");
                } else if (statement.type == STATEMENT_COMMIT) {
                    printf("COMMIT\n");
                } else if (statement.type == STATEMENT_ROLLBACK) {
                    printf("ROLLBACK\n");
                }
                break;
            case EXECUTE_DUPLICATE_KEY:
                printf("Error: Duplicate key.\n");
                break;
            case EXECUTE_TX_ALREADY_ACTIVE:
                printf("Error: a transaction is already active.\n");
                break;
            case EXECUTE_NO_ACTIVE_TX:
                printf("Error: no active transaction.\n");
                break;
            case EXECUTE_TABLE_FULL:
                printf("Error: table is full (max %d pages).\n", TABLE_MAX_PAGES);
                break;
            case EXECUTE_TABLE_NOT_FOUND:
            case EXECUTE_TABLE_ALREADY_EXISTS:
            case EXECUTE_TOO_MANY_TABLES:
            case EXECUTE_COLUMN_NOT_FOUND:
            case EXECUTE_COLUMN_ALREADY_EXISTS:
            case EXECUTE_TOO_MANY_COLUMNS:
            case EXECUTE_INVALID_OPERATION:
                break;
        }
    }

    database_close(db);
    return 0;
}
