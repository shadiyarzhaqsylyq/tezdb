#include "common.h"
#include "database.h"
#include "parser.h"
#include "executor.h"
#include "btree.h"
#include <unistd.h>
#include <ctype.h>



static void run_statement(const char* input, Database* db) {
    TokenList tokens;
    tokenize_input(input, &tokens);

    // Skip empty statements (e.g. stray semicolons)
    if (tokens.count == 0) return;

    Statement statement;
    PrepareResult prep_res = prepare_statement(&tokens, db, &statement);

    switch (prep_res) {
        case PREPARE_SUCCESS:
            break;
        case PREPARE_NEGATIVE_ID:
            printf("ID must be positive.\n");
            free_expr(statement.where);
            return;
        case PREPARE_STRING_TOO_LONG:
            printf("String is too long for column budget.\n");
            free_expr(statement.where);
            return;
        case PREPARE_SYNTAX_ERROR:
            printf("Syntax error. Could not parse statement.\n");
            free_expr(statement.where);
            return;
        case PREPARE_UNRECOGNIZED_STATEMENT:
            printf("Unrecognized keyword at start of '%s'.\n", input);
            free_expr(statement.where);
            return;
        case PREPARE_NO_SUCH_TABLE:
            printf("Error: no such table '%s'.\n", statement.table_name);
            free_expr(statement.where);
            return;
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
		case EXECUTE_FOREIGN_KEY_VIOLATION:
			printf("Error: foreign key constraint violation.\n");
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

static ssize_t find_semicolon(const char* str) {
    bool in_quotes = false;
    for (size_t i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\'') {
            in_quotes = !in_quotes;
        } else if (str[i] == ';' && !in_quotes) {
            return (ssize_t)i;
        }
    }
    return -1;
}

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

#define BATCH_BUFFER_SIZE 65536

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }

    const char* filename = argv[1];
    Database* db = database_open(filename);

    bool is_interactive = isatty(fileno(stdin));

    char line_buffer[1024];
    static char query_buffer[BATCH_BUFFER_SIZE];
    size_t query_len = 0;
    query_buffer[0] = '\0';

    while (true) {
        if (is_interactive) {
            // Show continuation prompt "db-# " if in the middle of a multi-line query
            printf(query_len == 0 ? "db=# " : "db-# ");
            fflush(stdout);
        }

        if (!fgets(line_buffer, sizeof(line_buffer), stdin)) {
            // End Of File reached: execute any remaining statement without a trailing semicolon
            if (query_len > 0) {
                run_statement(query_buffer, db);
            }
            break;
        }

        // 1. Strip trailing '\r' and '\n'
        size_t line_len = strlen(line_buffer);
        while (line_len > 0 && (line_buffer[line_len - 1] == '\n' || line_buffer[line_len - 1] == '\r')) {
            line_buffer[--line_len] = '\0';
        }

        // 2. Skip leading whitespace
        char* line = line_buffer;
        while (*line && isspace((unsigned char)*line)) {
            line++;
        }

        // 3. Skip empty lines or SQL comments
        if (*line == '\0' || (line[0] == '-' && line[1] == '-')) {
            continue;
        }

        // 4. Meta-commands (like .exit, \dt) execute immediately if no SQL is currently buffered
        if (query_len == 0 && (line[0] == '.' || line[0] == '\\')) {
            switch (do_meta_command(line, db)) {
                case META_COMMAND_SUCCESS:
                    continue;
                case META_COMMAND_UNRECOGNIZED_COMMAND:
                    printf("Unrecognized command '%s'\n", line);
                    continue;
            }
        }

        // 5. Append line to accumulator buffer
        if (query_len + strlen(line) + 2 < BATCH_BUFFER_SIZE) {
            if (query_len > 0) {
                query_buffer[query_len++] = ' '; // separator between lines
            }
            strcpy(&query_buffer[query_len], line);
            query_len += strlen(line);
        } else {
            printf("Error: SQL statement buffer exceeded.\n");
            query_len = 0;
            query_buffer[0] = '\0';
            continue;
        }

        // 6. Split and run all complete statements ending with ';'
        ssize_t semi_idx;
        while ((semi_idx = find_semicolon(query_buffer)) >= 0) {
            query_buffer[semi_idx] = '\0';

            run_statement(query_buffer, db);

            // Shift remaining characters forward
            size_t remaining = query_len - (semi_idx + 1);
            memmove(query_buffer, &query_buffer[semi_idx + 1], remaining);
            query_len = remaining;
            query_buffer[query_len] = '\0';

            // Trim leading spaces from the remaining buffer
            size_t start = 0;
            while (start < query_len && isspace((unsigned char)query_buffer[start])) {
                start++;
            }
            if (start > 0) {
                memmove(query_buffer, &query_buffer[start], query_len - start + 1);
                query_len -= start;
            }
        }
    }

    database_close(db);
    return 0;
}
