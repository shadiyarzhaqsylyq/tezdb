//#define _POSIX_C_SOURCE 200809L // for pread/pwrite/ftruncate declarations

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// Data Types & Schema Definition
// ============================================================================
#define PAGE_SIZE 4096
#define TABLE_MAX_PAGES 400
#define INVALID_PAGE_NUM UINT32_MAX
#define INSERT_PAGE_SAFETY_MARGIN 8
#define SCHEMA_MAGIC_V1_SINGLE_TABLE 0x5343484D    // legacy single-table format, no longer supported
#define SCHEMA_MAGIC_V2_NO_FREELIST 0x53434832     // multi-table format without page reclamation, no longer supported
#define SCHEMA_MAGIC 0x53434833                    // multi-table + free-list format

#define MAX_COLUMNS 32
#define MAX_NAME_LEN 64
#define MAX_STR_LEN 256
#define MAX_TOKENS 256
#define MAX_ASSIGNMENTS 32

// Multi-table catalog layout: page 0 is the DbHeader, pages
// [CATALOG_START_PAGE, CATALOG_START_PAGE + MAX_TABLES) each hold one
// TableCatalogEntry, and FREE_LIST_PAGE holds the list of reclaimed pages
// available for reuse. Actual table data (B+tree pages) is allocated after
// that, starting wherever the pager's page count naturally lands.
#define MAX_TABLES 16
#define CATALOG_START_PAGE 1
#define FREE_LIST_PAGE (CATALOG_START_PAGE + MAX_TABLES)

typedef enum DataType {
    DATA_TYPE_INT = 0,
    DATA_TYPE_VARCHAR = 1
} DataType;

typedef struct ColumnDef {
    char name[MAX_NAME_LEN];
    DataType type;
    uint32_t length; // For VARCHAR length; 0 for INT
    uint32_t offset; // Byte offset in row buffer
    uint32_t size;   // Size in row buffer
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

static void schema_add_column(Schema* schema, const char* name, DataType type, uint32_t length, bool is_pk) {
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
    } else { // VARCHAR
        col->length = (length > 0) ? length : 32;
        col->size = col->length + 1; // Null-terminated string buffer
    }

    if (is_pk) {
        schema->primary_key_index = schema->num_columns;
    }

    schema->row_size += col->size;
    schema->num_columns++;
    schema->has_schema = true;
}

static int strcasecmp_custom(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

static int schema_find_column(const Schema* schema, const char* name) {
    // Exact match: covers plain column names, and "table.col" qualified
    // names when the schema's own column names are stored that way (see
    // build_combined_schema, used for JOIN).
    for (uint32_t i = 0; i < schema->num_columns; ++i) {
        if (strcasecmp_custom(schema->columns[i].name, name) == 0) {
            return (int)i;
        }
    }

    const char* dot = strrchr(name, '.');
    if (dot) {
        // Reference is qualified (e.g. "emp.dept_id") but this schema
        // stores plain names -- match on the part after the dot.
        for (uint32_t i = 0; i < schema->num_columns; ++i) {
            if (strcasecmp_custom(schema->columns[i].name, dot + 1) == 0) {
                return (int)i;
            }
        }
        return -1;
    }

    // Reference is unqualified but this schema stores "table.col" names
    // (a combined JOIN schema) -- match on the suffix. Only succeeds if
    // exactly one column qualifies; an unqualified name matching columns
    // from both sides of a join is rejected as ambiguous rather than
    // guessed at.
    int found = -1;
    for (uint32_t i = 0; i < schema->num_columns; ++i) {
        const char* col_dot = strrchr(schema->columns[i].name, '.');
        if (!col_dot) continue;
        if (strcasecmp_custom(col_dot + 1, name) == 0) {
            if (found >= 0) return -1; // ambiguous
            found = (int)i;
        }
    }
    return found;
}

// One entry per table in the database's catalog. Lives at a fixed catalog
// page (see CATALOG_START_PAGE) so it survives restarts. schema.table_name
// is the table's name.
typedef struct TableCatalogEntry {
    bool in_use;
    Schema schema;
    uint32_t root_page_num;
} TableCatalogEntry;

// Builds a synthetic schema describing a JOINed row: left's columns
// followed by right's, each renamed "table.column" so schema_find_column
// can resolve both qualified references and unambiguous unqualified ones.
// Byte-layout fields (row_size/offset/primary_key_index) are left at 0 --
// this schema only ever describes an in-memory joined row, never one
// that's serialized to disk.
static void build_combined_schema(Schema* out, const Schema* left, const Schema* right) {
    // MAX_NAME_LEN*2 is large enough that "%s+%s"/"%s.%s" of two
    // MAX_NAME_LEN-1-byte names can never be truncated here -- the actual,
    // possibly-truncating copy into the fixed-size field happens via
    // strncpy below, which doesn't trip gcc's format-truncation warning.
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

// ============================================================================
// Dynamic Value & Row Structures
// ============================================================================

typedef struct Value {
    DataType type;
    int32_t int_val;
    char str_val[MAX_STR_LEN];
} Value;

typedef struct DynamicRow {
    Value values[MAX_COLUMNS];
    uint32_t num_values;
} DynamicRow;

static uint32_t get_pk_value(const DynamicRow* row, const Schema* schema) {
    if (schema->primary_key_index < row->num_values) {
        return (uint32_t)row->values[schema->primary_key_index].int_val;
    }
    return 0;
}

typedef enum ExecuteResult {
    EXECUTE_SUCCESS,
    EXECUTE_DUPLICATE_KEY,
    EXECUTE_TX_ALREADY_ACTIVE,
    EXECUTE_NO_ACTIVE_TX,
    EXECUTE_TABLE_FULL,
    EXECUTE_TABLE_NOT_FOUND,
    EXECUTE_TABLE_ALREADY_EXISTS,
    EXECUTE_TOO_MANY_TABLES,
    EXECUTE_COLUMN_NOT_FOUND,
    EXECUTE_COLUMN_ALREADY_EXISTS,
    EXECUTE_TOO_MANY_COLUMNS,
    EXECUTE_INVALID_OPERATION
} ExecuteResult;

typedef enum MetaCommandResult {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum PrepareResult {
    PREPARE_SUCCESS,
    PREPARE_NEGATIVE_ID,
    PREPARE_STRING_TOO_LONG,
    PREPARE_SYNTAX_ERROR,
    PREPARE_UNRECOGNIZED_STATEMENT,
    PREPARE_NO_SUCH_TABLE
} PrepareResult;

typedef enum StatementType {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_UPDATE,
    STATEMENT_DELETE,
    STATEMENT_CREATE,
    STATEMENT_DROP,
    STATEMENT_ALTER,
    STATEMENT_BEGIN,
    STATEMENT_COMMIT,
    STATEMENT_ROLLBACK
} StatementType;

typedef enum AlterKind {
    ALTER_ADD_COLUMN,
    ALTER_DROP_COLUMN,
    ALTER_RENAME_TABLE,
    ALTER_RENAME_COLUMN
} AlterKind;

// AGG_NONE must stay 0 so a zero-initialized Statement defaults to it.
typedef enum AggregateType {
    AGG_NONE,
    AGG_COUNT,
    AGG_SUM,
    AGG_AVG,
    AGG_MIN,
    AGG_MAX
} AggregateType;

typedef enum WhereOp {
    WHERE_OP_EQ,
    WHERE_OP_NEQ,
    WHERE_OP_GT,
    WHERE_OP_LT,
    WHERE_OP_GE,
    WHERE_OP_LE
} WhereOp;

typedef enum NodeType {
    NODE_INTERNAL = 0,
    NODE_LEAF = 1
} NodeType;

// ============================================================================
// Serialization Utilities
// ============================================================================

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

// ============================================================================
// WHERE-clause Structures & Evaluation (Tree with AND / OR / Parentheses)
// ============================================================================

typedef enum ExprType {
    EXPR_COMPARISON,
    EXPR_LOGICAL
} ExprType;

typedef enum LogicalOp {
    LOGICAL_AND,
    LOGICAL_OR
} LogicalOp;

typedef struct Literal {
    bool is_string;
    uint32_t int_value;
    char str_value[MAX_STR_LEN];
} Literal;

typedef struct Expr {
    ExprType type;

    // EXPR_COMPARISON fields
    char column[MAX_NAME_LEN];
    WhereOp op;
    Literal value;

    // EXPR_LOGICAL fields
    LogicalOp log_op;
    struct Expr* left;
    struct Expr* right;
} Expr;

void free_expr(Expr* expr) {
    if (!expr) return;
    if (expr->type == EXPR_LOGICAL) {
        free_expr(expr->left);
        free_expr(expr->right);
    }
    free(expr);
}

// Reverses a comparison operator's direction, e.g. for "a OP b" written as
// "b OP a" (used when a JOIN's ON clause has its operands swapped).
static WhereOp flip_op(WhereOp op) {
    switch (op) {
        case WHERE_OP_GT: return WHERE_OP_LT;
        case WHERE_OP_LT: return WHERE_OP_GT;
        case WHERE_OP_GE: return WHERE_OP_LE;
        case WHERE_OP_LE: return WHERE_OP_GE;
        default: return op; // EQ, NEQ are symmetric
    }
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

typedef struct UpdateAssignment {
    char column_name[MAX_NAME_LEN];
    char value_text[MAX_STR_LEN];
} UpdateAssignment;

typedef struct Statement {
    StatementType type;
    DynamicRow row_to_insert;
    char table_name[MAX_NAME_LEN];
    Schema created_schema;

    Expr* where;
    uint32_t target_id;

    // JOIN (inner join only, one FROM + one JOIN)
    bool has_join;
    char join_table_name[MAX_NAME_LEN];
    char join_left_column[MAX_NAME_LEN];  // unqualified name, in the FROM table
    char join_right_column[MAX_NAME_LEN]; // unqualified name, in the JOIN table
    WhereOp join_op;

    // SELECT modifiers
    AggregateType agg_type;         // AGG_NONE for a plain SELECT *
    char agg_column[MAX_NAME_LEN];  // column for SUM/AVG/MIN/MAX; unused otherwise
    bool has_order_by;
    char order_by_column[MAX_NAME_LEN];
    bool order_by_desc;
    bool has_limit;
    uint32_t limit_count;
    bool has_offset;
    uint32_t offset_count;

    // SET-style UPDATE
    bool is_set_update;
    UpdateAssignment update_assignments[MAX_ASSIGNMENTS];
    uint32_t num_update_assignments;

    // ALTER TABLE
    AlterKind alter_kind;
    char alter_column_name[MAX_NAME_LEN]; // column to add/drop/rename
    char alter_new_name[MAX_NAME_LEN];    // new table name, or new column name
    DataType alter_column_type;
    uint32_t alter_column_length;
} Statement;

// ============================================================================
// B+Tree Layout Helpers
// ============================================================================

#define NODE_TYPE_SIZE sizeof(uint8_t)
#define NODE_TYPE_OFFSET 0
#define IS_ROOT_SIZE sizeof(uint8_t)
#define IS_ROOT_OFFSET (NODE_TYPE_SIZE)
#define PARENT_POINTER_SIZE sizeof(uint32_t)
#define PARENT_POINTER_OFFSET (IS_ROOT_OFFSET + IS_ROOT_SIZE)
#define COMMON_NODE_HEADER_SIZE (NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE)

#define INTERNAL_NODE_NUM_KEYS_SIZE sizeof(uint32_t)
#define INTERNAL_NODE_NUM_KEYS_OFFSET (COMMON_NODE_HEADER_SIZE)
#define INTERNAL_NODE_RIGHT_CHILD_SIZE sizeof(uint32_t)
#define INTERNAL_NODE_RIGHT_CHILD_OFFSET (INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE)
#define INTERNAL_NODE_HEADER_SIZE (COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE + INTERNAL_NODE_RIGHT_CHILD_SIZE)

#define INTERNAL_NODE_KEY_SIZE sizeof(uint32_t)
#define INTERNAL_NODE_CHILD_SIZE sizeof(uint32_t)
#define INTERNAL_NODE_CELL_SIZE (INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE)
#define INTERNAL_NODE_MAX_KEYS 3

#define LEAF_NODE_NUM_CELLS_SIZE sizeof(uint32_t)
#define LEAF_NODE_NUM_CELLS_OFFSET (COMMON_NODE_HEADER_SIZE)
#define LEAF_NODE_NEXT_LEAF_SIZE sizeof(uint32_t)
#define LEAF_NODE_NEXT_LEAF_OFFSET (LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE)
#define LEAF_NODE_HEADER_SIZE (COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE + LEAF_NODE_NEXT_LEAF_SIZE)
#define LEAF_NODE_KEY_SIZE sizeof(uint32_t)

static inline uint8_t* node_ptr(void* node, size_t offset) {
    return (uint8_t*)node + offset;
}

static inline NodeType get_node_type(void* node) {
    return (NodeType)(*node_ptr(node, NODE_TYPE_OFFSET));
}

static inline void set_node_type(void* node, NodeType type) {
    *node_ptr(node, NODE_TYPE_OFFSET) = (uint8_t)type;
}

static inline bool is_node_root(void* node) {
    return (bool)(*node_ptr(node, IS_ROOT_OFFSET));
}

static inline void set_node_root(void* node, bool is_root) {
    *node_ptr(node, IS_ROOT_OFFSET) = (uint8_t)is_root;
}

static inline uint32_t* node_parent(void* node) {
    return (uint32_t*)node_ptr(node, PARENT_POINTER_OFFSET);
}

static inline uint32_t* internal_node_num_keys(void* node) {
    return (uint32_t*)node_ptr(node, INTERNAL_NODE_NUM_KEYS_OFFSET);
}

static inline uint32_t* internal_node_right_child(void* node) {
    return (uint32_t*)node_ptr(node, INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

static inline uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
    return (uint32_t*)node_ptr(node, INTERNAL_NODE_HEADER_SIZE + cell_num * INTERNAL_NODE_CELL_SIZE);
}

static inline uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) {
        printf("Tried to access child_num %u > num_keys %u\n", child_num, num_keys);
        exit(EXIT_FAILURE);
    } else if (child_num == num_keys) {
        uint32_t* right_child = internal_node_right_child(node);
        if (*right_child == INVALID_PAGE_NUM) {
            printf("Tried to access right child of node, but was invalid page\n");
            exit(EXIT_FAILURE);
        }
        return right_child;
    } else {
        uint32_t* child = internal_node_cell(node, child_num);
        if (*child == INVALID_PAGE_NUM) {
            printf("Tried to access child %u of node, but was invalid page\n", child_num);
            exit(EXIT_FAILURE);
        }
        return child;
    }
}

static inline uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return (uint32_t*)node_ptr(internal_node_cell(node, key_num), INTERNAL_NODE_CHILD_SIZE);
}

static inline uint32_t leaf_node_cell_size(uint32_t row_size) {
    return LEAF_NODE_KEY_SIZE + row_size;
}

static inline uint32_t leaf_node_max_cells(uint32_t row_size) {
    if (row_size == 0) return 0;
    uint32_t space = PAGE_SIZE - LEAF_NODE_HEADER_SIZE;
    return space / leaf_node_cell_size(row_size);
}

static inline uint32_t* leaf_node_num_cells(void* node) {
    return (uint32_t*)node_ptr(node, LEAF_NODE_NUM_CELLS_OFFSET);
}

static inline uint32_t* leaf_node_next_leaf(void* node) {
    return (uint32_t*)node_ptr(node, LEAF_NODE_NEXT_LEAF_OFFSET);
}

static inline void* leaf_node_cell(void* node, uint32_t cell_num, uint32_t row_size) {
    return node_ptr(node, LEAF_NODE_HEADER_SIZE + cell_num * leaf_node_cell_size(row_size));
}

static inline uint32_t* leaf_node_key(void* node, uint32_t cell_num, uint32_t row_size) {
    return (uint32_t*)leaf_node_cell(node, cell_num, row_size);
}

static inline void* leaf_node_value(void* node, uint32_t cell_num, uint32_t row_size) {
    return node_ptr(leaf_node_cell(node, cell_num, row_size), LEAF_NODE_KEY_SIZE);
}

// ============================================================================
// Pager Structure & Functions
// ============================================================================

typedef struct Pager {
    int file_descriptor;
    uint32_t file_length;
    uint32_t num_pages;
    void* pages[TABLE_MAX_PAGES];
} Pager;

Pager* pager_open(const char* filename) {
    int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    if (fd == -1) {
        printf("Unable to open file\n");
        exit(EXIT_FAILURE);
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        printf("Error obtaining file stats: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    Pager* pager = (Pager*)malloc(sizeof(Pager));
    pager->file_descriptor = fd;
    pager->file_length = (uint32_t)st.st_size;
    pager->num_pages = pager->file_length / PAGE_SIZE;

    if (pager->file_length % PAGE_SIZE != 0) {
        printf("Db file is not a whole number of pages. Corrupt file.\n");
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->pages[i] = NULL;
    }

    return pager;
}

void* pager_get_page(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) {
        printf("Tried to fetch page number out of bounds. %u >= %d\n", page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    if (pager->pages[page_num] == NULL) {
        void* page = malloc(PAGE_SIZE);
        uint32_t npages = pager->file_length / PAGE_SIZE;
        if (pager->file_length % PAGE_SIZE) {
            npages += 1;
        }
        if (page_num <= npages) {
            off_t offset = (off_t)page_num * PAGE_SIZE;
            ssize_t bytes_read = pread(pager->file_descriptor, page, PAGE_SIZE, offset);
            if (bytes_read == -1) {
                printf("Error reading file: %d\n", errno);
                exit(EXIT_FAILURE);
            }
        }
        pager->pages[page_num] = page;
        if (page_num >= pager->num_pages) {
            pager->num_pages = page_num + 1;
        }
    }
    return pager->pages[page_num];
}

void pager_flush(Pager* pager, uint32_t page_num) {
    if (pager->pages[page_num] == NULL) {
        printf("Tried to flush null page\n");
        exit(EXIT_FAILURE);
    }

    off_t offset = (off_t)page_num * PAGE_SIZE;
    ssize_t bytes_written = pwrite(pager->file_descriptor, pager->pages[page_num], PAGE_SIZE, offset);
    if (bytes_written == -1) {
        printf("Error writing: %d\n", errno);
        exit(EXIT_FAILURE);
    }
}

void pager_close(Pager* pager) {
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (pager->pages[i] != NULL) {
            free(pager->pages[i]);
            pager->pages[i] = NULL;
        }
    }
    if (pager->file_descriptor != -1) {
        close(pager->file_descriptor);
    }
    free(pager);
}

// ============================================================================
// Table & Cursor Structures & B+Tree Operations
// ============================================================================

typedef struct Database Database; // forward declaration; full definition below

// A lightweight view over one table's data within the shared pager. Built
// on demand (from a Database's catalog entry) for whichever table a
// statement targets; it does not own the pager. `db` is set so that page
// allocation (e.g. during a B+tree split) can draw from the shared
// free list.
typedef struct Table {
    Pager* pager;
    uint32_t root_page_num;
    Schema schema;
    Database* db;
} Table;

// The database file as a whole: the shared pager, the table catalog,
// reclaimed-page free list, and transaction state (transactions span the
// whole file, not one table).
struct Database {
    Pager* pager;

    TableCatalogEntry tables[MAX_TABLES];
    uint32_t num_tables;

    // Pages freed by DROP TABLE / ALTER TABLE migrations, available for
    // reuse before the file is grown further.
    uint32_t free_pages[TABLE_MAX_PAGES];
    uint32_t num_free_pages;

    // Transaction state
    bool in_transaction;
    uint32_t tx_original_num_pages;
    void* tx_backup[TABLE_MAX_PAGES];
    bool tx_was_cached[TABLE_MAX_PAGES];
};

typedef struct Cursor {
    Table* table;
    uint32_t page_num;
    uint32_t cell_num;
    bool end_of_table;
} Cursor;

void initialize_leaf_node(void* node) {
    set_node_type(node, NODE_LEAF);
    set_node_root(node, false);
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = 0;
}

void initialize_internal_node(void* node) {
    set_node_type(node, NODE_INTERNAL);
    set_node_root(node, false);
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

void tx_free_backups(Database* db) {
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (db->tx_backup[i] != NULL) {
            free(db->tx_backup[i]);
            db->tx_backup[i] = NULL;
        }
    }
}

// Page 0. Points to nothing else; the catalog entries live at fixed pages
// starting at CATALOG_START_PAGE (see database_open).
// Page 0. Points to nothing else; the catalog entries live at fixed pages
// starting at CATALOG_START_PAGE (see database_open).
typedef struct DbHeader {
    uint32_t magic;
    uint32_t num_tables;
} DbHeader;

// Lives at FREE_LIST_PAGE. Holds every reclaimed page number available for
// reuse. TABLE_MAX_PAGES is a safe upper bound on how many pages the whole
// file (and thus the free list) could ever contain.
typedef struct FreeListPage {
    uint32_t count;
    uint32_t pages[TABLE_MAX_PAGES];
} FreeListPage;

// ----------------------------------------------------------------------
// Catalog & free-list persistence: each lives at its own fixed page(s),
// the same way the old single-table MetaPage worked.
// ----------------------------------------------------------------------

static void persist_catalog_entry(Database* db, uint32_t slot) {
    TableCatalogEntry* page = (TableCatalogEntry*)pager_get_page(db->pager, CATALOG_START_PAGE + slot);
    *page = db->tables[slot];
}

static void persist_header(Database* db) {
    DbHeader* header = (DbHeader*)pager_get_page(db->pager, 0);
    header->magic = SCHEMA_MAGIC;
    header->num_tables = db->num_tables;
}

static void persist_free_list(Database* db) {
    FreeListPage* page = (FreeListPage*)pager_get_page(db->pager, FREE_LIST_PAGE);
    page->count = db->num_free_pages;
    memcpy(page->pages, db->free_pages, db->num_free_pages * sizeof(uint32_t));
}

// Re-reads the catalog from disk into db->tables[]/db->num_tables. Used
// after ROLLBACK, since rollback restores raw pages but not this in-memory
// cache.
static void reload_catalog_from_disk(Database* db) {
    DbHeader* header = (DbHeader*)pager_get_page(db->pager, 0);
    db->num_tables = header->num_tables;
    for (uint32_t i = 0; i < MAX_TABLES; i++) {
        TableCatalogEntry* page = (TableCatalogEntry*)pager_get_page(db->pager, CATALOG_START_PAGE + i);
        db->tables[i] = *page;
    }
}

static void reload_free_list_from_disk(Database* db) {
    FreeListPage* page = (FreeListPage*)pager_get_page(db->pager, FREE_LIST_PAGE);
    db->num_free_pages = page->count;
    memcpy(db->free_pages, page->pages, db->num_free_pages * sizeof(uint32_t));
}

// Hands out a page number for new table data: reuses a reclaimed page if
// one is available, otherwise grows the file by one page.
static uint32_t allocate_page(Database* db) {
    uint32_t page_num;
    if (db->num_free_pages > 0) {
        page_num = db->free_pages[--db->num_free_pages];
    } else {
        page_num = db->pager->num_pages;
    }
    persist_free_list(db);
    return page_num;
}

static void free_page(Database* db, uint32_t page_num) {
    if (db->num_free_pages < TABLE_MAX_PAGES) {
        db->free_pages[db->num_free_pages++] = page_num;
    }
}

// Recursively collects every page belonging to one table's B+tree (root,
// internal nodes, and leaves) into out_pages. Used to reclaim a table's
// pages on DROP TABLE or after an ALTER TABLE migration.
static void collect_table_pages(Database* db, uint32_t page_num, uint32_t* out_pages, uint32_t* out_count) {
    if (page_num == INVALID_PAGE_NUM) return;
    void* node = pager_get_page(db->pager, page_num);
    if (get_node_type(node) == NODE_INTERNAL) {
        uint32_t num_keys = *internal_node_num_keys(node);
        for (uint32_t i = 0; i < num_keys; i++) {
            collect_table_pages(db, *internal_node_child(node, i), out_pages, out_count);
        }
        collect_table_pages(db, *internal_node_right_child(node), out_pages, out_count);
    }
    if (*out_count < TABLE_MAX_PAGES) {
        out_pages[(*out_count)++] = page_num;
    }
}

static int database_find_table_index(const Database* db, const char* name) {
    for (uint32_t i = 0; i < MAX_TABLES; i++) {
        if (db->tables[i].in_use && strcasecmp_custom(db->tables[i].schema.table_name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static const TableCatalogEntry* database_find_table(const Database* db, const char* name) {
    int idx = database_find_table_index(db, name);
    return idx < 0 ? NULL : &db->tables[idx];
}

Database* database_open(const char* filename) {
    Database* db = (Database*)malloc(sizeof(Database));
    db->pager = pager_open(filename);
    db->in_transaction = false;
    db->tx_original_num_pages = 0;
    db->num_tables = 0;
    db->num_free_pages = 0;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        db->tx_backup[i] = NULL;
        db->tx_was_cached[i] = false;
    }
    for (uint32_t i = 0; i < MAX_TABLES; i++) {
        memset(&db->tables[i], 0, sizeof(TableCatalogEntry));
    }

    if (db->pager->num_pages == 0) {
        // Fresh file: write the header, an empty catalog, and an empty
        // free list. No tables exist yet; CREATE TABLE allocates data
        // pages after all of that.
        DbHeader* header = (DbHeader*)pager_get_page(db->pager, 0);
        header->magic = SCHEMA_MAGIC;
        header->num_tables = 0;

        for (uint32_t i = 0; i < MAX_TABLES; i++) {
            TableCatalogEntry* page = (TableCatalogEntry*)pager_get_page(db->pager, CATALOG_START_PAGE + i);
            memset(page, 0, sizeof(TableCatalogEntry));
        }

        FreeListPage* free_list = (FreeListPage*)pager_get_page(db->pager, FREE_LIST_PAGE);
        free_list->count = 0;
    } else {
        DbHeader* header = (DbHeader*)pager_get_page(db->pager, 0);
        if (header->magic == SCHEMA_MAGIC_V1_SINGLE_TABLE) {
            printf("Error: '%s' uses an older single-table format and cannot be opened by this version.\n", filename);
            exit(EXIT_FAILURE);
        }
        if (header->magic == SCHEMA_MAGIC_V2_NO_FREELIST) {
            printf("Error: '%s' uses an older multi-table format without page reclamation and cannot be opened by this version.\n", filename);
            exit(EXIT_FAILURE);
        }
        if (header->magic != SCHEMA_MAGIC) {
            printf("Db file has an unrecognized header. Corrupt file.\n");
            exit(EXIT_FAILURE);
        }
        reload_catalog_from_disk(db);
        reload_free_list_from_disk(db);
    }
    return db;
}


void database_close(Database* db) {
    tx_free_backups(db);
    for (uint32_t i = 0; i < db->pager->num_pages; i++) {
        if (db->pager->pages[i] == NULL) continue;
        pager_flush(db->pager, i);
        free(db->pager->pages[i]);
        db->pager->pages[i] = NULL;
    }
    pager_close(db->pager);
    free(db);
}

uint32_t get_node_max_key(Table* table, void* node) {
    if (get_node_type(node) == NODE_LEAF) {
        return *leaf_node_key(node, *leaf_node_num_cells(node) - 1, table->schema.row_size);
    }
    void* right_child = pager_get_page(table->pager, *internal_node_right_child(node));
    return get_node_max_key(table, right_child);
}

Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void* node = pager_get_page(table->pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    Cursor* cursor = (Cursor*)malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->page_num = page_num;
    cursor->end_of_table = false;

    uint32_t min_index = 0;
    uint32_t one_past_max_index = num_cells;
    while (one_past_max_index != min_index) {
        uint32_t index = (min_index + one_past_max_index) / 2;
        uint32_t key_at_index = *leaf_node_key(node, index, table->schema.row_size);
        if (key == key_at_index) {
            cursor->cell_num = index;
            return cursor;
        }
        if (key < key_at_index) {
            one_past_max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    cursor->cell_num = min_index;
    return cursor;
}

uint32_t internal_node_find_child(void* node, uint32_t key) {
    uint32_t num_keys = *internal_node_num_keys(node);
    uint32_t min_index = 0;
    uint32_t max_index = num_keys;
    while (min_index != max_index) {
        uint32_t index = (min_index + max_index) / 2;
        uint32_t key_to_right = *internal_node_key(node, index);
        if (key_to_right >= key) {
            max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return min_index;
}

Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void* node = pager_get_page(table->pager, page_num);
    uint32_t child_index = internal_node_find_child(node, key);
    uint32_t child_num = *internal_node_child(node, child_index);
    void* child = pager_get_page(table->pager, child_num);
    switch (get_node_type(child)) {
        case NODE_LEAF:
            return leaf_node_find(table, child_num, key);
        case NODE_INTERNAL:
            return internal_node_find(table, child_num, key);
    }
    return NULL;
}

Cursor* table_find(Table* table, uint32_t key) {
    void* root_node = pager_get_page(table->pager, table->root_page_num);
    if (get_node_type(root_node) == NODE_LEAF) {
        return leaf_node_find(table, table->root_page_num, key);
    } else {
        return internal_node_find(table, table->root_page_num, key);
    }
}

Cursor* table_start(Table* table) {
    Cursor* cursor = table_find(table, 0);
    void* node = pager_get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    cursor->end_of_table = (num_cells == 0);
    return cursor;
}

void* cursor_value(Cursor* cursor) {
    void* page = pager_get_page(cursor->table->pager, cursor->page_num);
    return leaf_node_value(page, cursor->cell_num, cursor->table->schema.row_size);
}

void cursor_advance(Cursor* cursor) {
    void* node = pager_get_page(cursor->table->pager, cursor->page_num);
    cursor->cell_num += 1;
    if (cursor->cell_num >= (*leaf_node_num_cells(node))) {
        uint32_t next_page_num = *leaf_node_next_leaf(node);
        if (next_page_num == 0) {
            cursor->end_of_table = true;
        } else {
            cursor->page_num = next_page_num;
            cursor->cell_num = 0;
        }
    }
}

void leaf_node_delete(Cursor* cursor) {
    void* node = pager_get_page(cursor->table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    uint32_t cell_sz = leaf_node_cell_size(cursor->table->schema.row_size);
    for (uint32_t i = cursor->cell_num; i < num_cells - 1; i++) {
        memcpy(leaf_node_cell(node, i, cursor->table->schema.row_size),
               leaf_node_cell(node, i + 1, cursor->table->schema.row_size), cell_sz);
    }
    *(leaf_node_num_cells(node)) -= 1;
}

uint32_t get_unused_page_num(Table* table) {
    return allocate_page(table->db);
}

void create_new_root(Table* table, uint32_t right_child_page_num) {
    void* root = pager_get_page(table->pager, table->root_page_num);
    void* right_child = pager_get_page(table->pager, right_child_page_num);
    uint32_t left_child_page_num = get_unused_page_num(table);
    void* left_child = pager_get_page(table->pager, left_child_page_num);

    if (get_node_type(root) == NODE_INTERNAL) {
        initialize_internal_node(right_child);
        initialize_internal_node(left_child);
    }

    memcpy(left_child, root, PAGE_SIZE);
    set_node_root(left_child, false);

    if (get_node_type(left_child) == NODE_INTERNAL) {
        void* child;
        for (uint32_t i = 0; i < *internal_node_num_keys(left_child); i++) {
            child = pager_get_page(table->pager, *internal_node_child(left_child, i));
            *node_parent(child) = left_child_page_num;
        }
        child = pager_get_page(table->pager, *internal_node_right_child(left_child));
        *node_parent(child) = left_child_page_num;
    }

    initialize_internal_node(root);
    set_node_root(root, true);
    *internal_node_num_keys(root) = 1;
    *internal_node_child(root, 0) = left_child_page_num;
    uint32_t left_child_max_key = get_node_max_key(table, left_child);
    *internal_node_key(root, 0) = left_child_max_key;
    *internal_node_right_child(root) = right_child_page_num;
    *node_parent(left_child) = table->root_page_num;
    *node_parent(right_child) = table->root_page_num;
}

void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
    uint32_t old_child_index = internal_node_find_child(node, old_key);
    *internal_node_key(node, old_child_index) = new_key;
}

void internal_node_split_and_insert(Table* table, uint32_t parent_page_num, uint32_t child_page_num);

void internal_node_insert(Table* table, uint32_t parent_page_num, uint32_t child_page_num) {
    void* parent = pager_get_page(table->pager, parent_page_num);
    void* child = pager_get_page(table->pager, child_page_num);
    uint32_t child_max_key = get_node_max_key(table, child);
    uint32_t index = internal_node_find_child(parent, child_max_key);

    uint32_t original_num_keys = *internal_node_num_keys(parent);
    if (original_num_keys >= INTERNAL_NODE_MAX_KEYS) {
        internal_node_split_and_insert(table, parent_page_num, child_page_num);
        return;
    }

    uint32_t right_child_page_num = *internal_node_right_child(parent);
    if (right_child_page_num == INVALID_PAGE_NUM) {
        *internal_node_right_child(parent) = child_page_num;
        return;
    }

    void* right_child = pager_get_page(table->pager, right_child_page_num);
    *internal_node_num_keys(parent) = original_num_keys + 1;

    if (child_max_key > get_node_max_key(table, right_child)) {
        *internal_node_child(parent, original_num_keys) = right_child_page_num;
        *internal_node_key(parent, original_num_keys) = get_node_max_key(table, right_child);
        *internal_node_right_child(parent) = child_page_num;
    } else {
        for (uint32_t i = original_num_keys; i > index; i--) {
            void* destination = internal_node_cell(parent, i);
            void* source = internal_node_cell(parent, i - 1);
            memcpy(destination, source, INTERNAL_NODE_CELL_SIZE);
        }
        *internal_node_child(parent, index) = child_page_num;
        *internal_node_key(parent, index) = child_max_key;
    }
}

void internal_node_split_and_insert(Table* table, uint32_t parent_page_num, uint32_t child_page_num) {
    uint32_t old_page_num = parent_page_num;
    void* old_node = pager_get_page(table->pager, parent_page_num);
    uint32_t old_max = get_node_max_key(table, old_node);

    void* child = pager_get_page(table->pager, child_page_num);
    uint32_t child_max = get_node_max_key(table, child);

    uint32_t new_page_num = get_unused_page_num(table);

    uint32_t splitting_root = is_node_root(old_node);
    void* parent;
    void* new_node = NULL;

    if (splitting_root) {
        create_new_root(table, new_page_num);
        parent = pager_get_page(table->pager, table->root_page_num);
        old_page_num = *internal_node_child(parent, 0);
        old_node = pager_get_page(table->pager, old_page_num);
    } else {
        parent = pager_get_page(table->pager, *node_parent(old_node));
        new_node = pager_get_page(table->pager, new_page_num);
        initialize_internal_node(new_node);
    }

    uint32_t* old_num_keys = internal_node_num_keys(old_node);
    uint32_t cur_page_num = *internal_node_right_child(old_node);
    void* cur = pager_get_page(table->pager, cur_page_num);

    internal_node_insert(table, new_page_num, cur_page_num);
    *node_parent(cur) = new_page_num;
    *internal_node_right_child(old_node) = INVALID_PAGE_NUM;

    for (int i = INTERNAL_NODE_MAX_KEYS - 1; i > (int)(INTERNAL_NODE_MAX_KEYS / 2); i--) {
        cur_page_num = *internal_node_child(old_node, i);
        cur = pager_get_page(table->pager, cur_page_num);
        internal_node_insert(table, new_page_num, cur_page_num);
        *node_parent(cur) = new_page_num;
        (*old_num_keys)--;
    }

    *internal_node_right_child(old_node) = *internal_node_child(old_node, *old_num_keys - 1);
    (*old_num_keys)--;

    uint32_t max_after_split = get_node_max_key(table, old_node);
    uint32_t destination_page_num = child_max < max_after_split ? old_page_num : new_page_num;

    internal_node_insert(table, destination_page_num, child_page_num);
    *node_parent(child) = destination_page_num;

    update_internal_node_key(parent, old_max, get_node_max_key(table, old_node));

    if (!splitting_root) {
        internal_node_insert(table, *node_parent(old_node), new_page_num);
        *node_parent(new_node) = *node_parent(old_node);
    }
}

void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, DynamicRow* value) {
    Table* table = cursor->table;
    void* old_node = pager_get_page(table->pager, cursor->page_num);
    uint32_t old_max = get_node_max_key(table, old_node);
    uint32_t new_page_num = get_unused_page_num(table);
    void* new_node = pager_get_page(table->pager, new_page_num);

    initialize_leaf_node(new_node);
    *node_parent(new_node) = *node_parent(old_node);
    *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(old_node) = new_page_num;

    uint32_t max_cells = leaf_node_max_cells(table->schema.row_size);
    uint32_t right_split_count = (max_cells + 1) / 2;
    uint32_t left_split_count = (max_cells + 1) - right_split_count;
    uint32_t cell_sz = leaf_node_cell_size(table->schema.row_size);

    for (int32_t i = max_cells; i >= 0; i--) {
        void* destination_node;
        if ((uint32_t)i >= left_split_count) {
            destination_node = new_node;
        } else {
            destination_node = old_node;
        }
        uint32_t index_within_node = i % left_split_count;
        void* destination = leaf_node_cell(destination_node, index_within_node, table->schema.row_size);

        if ((uint32_t)i == cursor->cell_num) {
            serialize_row(value, leaf_node_value(destination_node, index_within_node, table->schema.row_size), &table->schema);
            *leaf_node_key(destination_node, index_within_node, table->schema.row_size) = key;
        } else if ((uint32_t)i > cursor->cell_num) {
            memcpy(destination, leaf_node_cell(old_node, i - 1, table->schema.row_size), cell_sz);
        } else {
            memcpy(destination, leaf_node_cell(old_node, i, table->schema.row_size), cell_sz);
        }
    }

    *(leaf_node_num_cells(old_node)) = left_split_count;
    *(leaf_node_num_cells(new_node)) = right_split_count;

    if (is_node_root(old_node)) {
        create_new_root(table, new_page_num);
    } else {
        uint32_t parent_page_num = *node_parent(old_node);
        uint32_t new_max = get_node_max_key(table, old_node);
        void* parent = pager_get_page(table->pager, parent_page_num);
        update_internal_node_key(parent, old_max, new_max);
        internal_node_insert(table, parent_page_num, new_page_num);
    }
}

void leaf_node_insert(Cursor* cursor, uint32_t key, DynamicRow* value) {
    Table* table = cursor->table;
    void* node = pager_get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    uint32_t max_cells = leaf_node_max_cells(table->schema.row_size);

    if (num_cells >= max_cells) {
        leaf_node_split_and_insert(cursor, key, value);
        return;
    }
    uint32_t cell_sz = leaf_node_cell_size(table->schema.row_size);
    if (cursor->cell_num < num_cells) {
        for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
            memcpy(leaf_node_cell(node, i, table->schema.row_size),
                   leaf_node_cell(node, i - 1, table->schema.row_size), cell_sz);
        }
    }
    *(leaf_node_num_cells(node)) += 1;
    *(leaf_node_key(node, cursor->cell_num, table->schema.row_size)) = key;
    serialize_row(value, leaf_node_value(node, cursor->cell_num, table->schema.row_size), &table->schema);
}

void print_tree(Table* table, uint32_t page_num, uint32_t indentation_level) {
    void* node = pager_get_page(table->pager, page_num);
    uint32_t num_keys, child;

    for (uint32_t i = 0; i < indentation_level; i++) printf("  ");

    switch (get_node_type(node)) {
        case NODE_LEAF:
            num_keys = *leaf_node_num_cells(node);
            printf("- leaf (size %u)\n", num_keys);
            for (uint32_t i = 0; i < num_keys; i++) {
                for (uint32_t j = 0; j < indentation_level + 1; j++) printf("  ");
                printf("- %u\n", *leaf_node_key(node, i, table->schema.row_size));
            }
            break;
        case NODE_INTERNAL:
            num_keys = *internal_node_num_keys(node);
            printf("- internal (size %u)\n", num_keys);
            if (num_keys > 0) {
                for (uint32_t i = 0; i < num_keys; i++) {
                    child = *internal_node_child(node, i);
                    print_tree(table, child, indentation_level + 1);
                    for (uint32_t j = 0; j < indentation_level + 1; j++) printf("  ");
                    printf("- key %u\n", *internal_node_key(node, i));
                }
                child = *internal_node_right_child(node);
                print_tree(table, child, indentation_level + 1);
            }
            break;
    }
}

// ============================================================================
// SQL Lexer & Parser
// ============================================================================

typedef enum TokenKind {
    TOKEN_IDENTIFIER,
    TOKEN_STRING_LITERAL,
    TOKEN_NUMBER,
    TOKEN_SYMBOL,
    TOKEN_END
} TokenKind;

typedef struct Token {
    TokenKind kind;
    char text[MAX_STR_LEN];
} Token;

typedef struct TokenList {
    Token tokens[MAX_TOKENS];
    uint32_t count;
    uint32_t cursor;
} TokenList;

void tokenize_input(const char* input, TokenList* list) {
    list->count = 0;
    list->cursor = 0;
    size_t pos = 0;
    size_t len = strlen(input);

    while (pos < len && list->count < MAX_TOKENS - 1) {
        if (isspace((unsigned char)input[pos])) {
            pos++;
            continue;
        }

        if (input[pos] == ';') {
            list->tokens[list->count].kind = TOKEN_SYMBOL;
            strcpy(list->tokens[list->count].text, ";");
            list->count++;
            pos++;
            continue;
        }

        if (input[pos] == '\'') {
            char str[MAX_STR_LEN] = {0};
            size_t str_pos = 0;
            pos++;
            while (pos < len && input[pos] != '\'' && str_pos < MAX_STR_LEN - 1) {
                str[str_pos++] = input[pos++];
            }
            str[str_pos] = '\0';
            if (pos < len && input[pos] == '\'') pos++;

            list->tokens[list->count].kind = TOKEN_STRING_LITERAL;
            strcpy(list->tokens[list->count].text, str);
            list->count++;
            continue;
        }

        // Two-character operators: <=, >=, !=, <>
        if (pos + 1 < len) {
            char op2[3] = {input[pos], input[pos + 1], '\0'};
            if (strcmp(op2, "<=") == 0 || strcmp(op2, ">=") == 0 ||
                strcmp(op2, "!=") == 0 || strcmp(op2, "<>") == 0) {
                list->tokens[list->count].kind = TOKEN_SYMBOL;
                strcpy(list->tokens[list->count].text, op2);
                list->count++;
                pos += 2;
                continue;
            }
        }

        if (strchr("=<>(),*", input[pos]) != NULL) {
            list->tokens[list->count].kind = TOKEN_SYMBOL;
            list->tokens[list->count].text[0] = input[pos];
            list->tokens[list->count].text[1] = '\0';
            list->count++;
            pos++;
            continue;
        }

        if (isdigit((unsigned char)input[pos])) {
            char num[MAX_STR_LEN] = {0};
            size_t num_pos = 0;
            while (pos < len && isdigit((unsigned char)input[pos]) && num_pos < MAX_STR_LEN - 1) {
                num[num_pos++] = input[pos++];
            }
            num[num_pos] = '\0';
            list->tokens[list->count].kind = TOKEN_NUMBER;
            strcpy(list->tokens[list->count].text, num);
            list->count++;
            continue;
        }

        if (isalpha((unsigned char)input[pos]) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\') {
            char ident[MAX_STR_LEN] = {0};
            size_t ident_pos = 0;
            while (pos < len && (isalnum((unsigned char)input[pos]) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\') && ident_pos < MAX_STR_LEN - 1) {
                ident[ident_pos++] = input[pos++];
            }
            ident[ident_pos] = '\0';
            list->tokens[list->count].kind = TOKEN_IDENTIFIER;
            strcpy(list->tokens[list->count].text, ident);
            list->count++;
            continue;
        }

        pos++;
    }

    list->tokens[list->count].kind = TOKEN_END;
    list->tokens[list->count].text[0] = '\0';
}

static Token peek_token(const TokenList* list) {
    return list->tokens[list->cursor];
}

static Token advance_token(TokenList* list) {
    return list->tokens[list->cursor++];
}

static bool match_token(TokenList* list, const char* text) {
    if (strcasecmp_custom(peek_token(list).text, text) == 0) {
        list->cursor++;
        return true;
    }
    return false;
}

// Parses a single comparison: <column> <op> <value>
PrepareResult parse_comparison(TokenList* list, const Schema* schema, Expr** out) {
    Token col = advance_token(list);
    Token op = advance_token(list);
    Token val = advance_token(list);

    if (col.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

    int col_idx = schema_find_column(schema, col.text);
    if (col_idx < 0) return PREPARE_SYNTAX_ERROR;

    Expr* cmp = (Expr*)calloc(1, sizeof(Expr));
    cmp->type = EXPR_COMPARISON;
    strncpy(cmp->column, schema->columns[col_idx].name, MAX_NAME_LEN - 1);

    if (strcmp(op.text, "=") == 0) cmp->op = WHERE_OP_EQ;
    else if (strcmp(op.text, "!=") == 0 || strcmp(op.text, "<>") == 0) cmp->op = WHERE_OP_NEQ;
    else if (strcmp(op.text, ">=") == 0) cmp->op = WHERE_OP_GE;
    else if (strcmp(op.text, "<=") == 0) cmp->op = WHERE_OP_LE;
    else if (strcmp(op.text, ">") == 0) cmp->op = WHERE_OP_GT;
    else if (strcmp(op.text, "<") == 0) cmp->op = WHERE_OP_LT;
    else {
        free(cmp);
        return PREPARE_SYNTAX_ERROR;
    }

    if (schema->columns[col_idx].type == DATA_TYPE_INT) {
        cmp->value.is_string = false;
        char* endptr;
        long int_v = strtol(val.text, &endptr, 10);
        if (*endptr != '\0') {
            free(cmp);
            return PREPARE_SYNTAX_ERROR;
        }
        if (schema->columns[col_idx].is_primary_key && int_v < 0) {
            free(cmp);
            return PREPARE_NEGATIVE_ID;
        }
        cmp->value.int_value = (uint32_t)int_v;
    } else {
        cmp->value.is_string = true;
        strncpy(cmp->value.str_value, val.text, MAX_STR_LEN - 1);
    }

    *out = cmp;
    return PREPARE_SUCCESS;
}

// Forward declarations for recursive descent parsing
PrepareResult parse_expr(TokenList* list, const Schema* schema, Expr** out);

PrepareResult parse_primary(TokenList* list, const Schema* schema, Expr** out) {
    if (peek_token(list).kind == TOKEN_SYMBOL && strcmp(peek_token(list).text, "(") == 0) {
        advance_token(list); // consume '('
        PrepareResult res = parse_expr(list, schema, out);
        if (res != PREPARE_SUCCESS) return res;
        if (peek_token(list).kind != TOKEN_SYMBOL || strcmp(peek_token(list).text, ")") != 0) {
            free_expr(*out);
            *out = NULL;
            return PREPARE_SYNTAX_ERROR;
        }
        advance_token(list); // consume ')'
        return PREPARE_SUCCESS;
    }
    return parse_comparison(list, schema, out);
}

PrepareResult parse_and(TokenList* list, const Schema* schema, Expr** out) {
    Expr* left = NULL;
    PrepareResult res = parse_primary(list, schema, &left);
    if (res != PREPARE_SUCCESS) return res;

    while (match_token(list, "AND")) {
        Expr* right = NULL;
        res = parse_primary(list, schema, &right);
        if (res != PREPARE_SUCCESS) {
            free_expr(left);
            return res;
        }
        Expr* parent = (Expr*)calloc(1, sizeof(Expr));
        parent->type = EXPR_LOGICAL;
        parent->log_op = LOGICAL_AND;
        parent->left = left;
        parent->right = right;
        left = parent;
    }
    *out = left;
    return PREPARE_SUCCESS;
}

PrepareResult parse_or(TokenList* list, const Schema* schema, Expr** out) {
    Expr* left = NULL;
    PrepareResult res = parse_and(list, schema, &left);
    if (res != PREPARE_SUCCESS) return res;

    while (match_token(list, "OR")) {
        Expr* right = NULL;
        res = parse_and(list, schema, &right);
        if (res != PREPARE_SUCCESS) {
            free_expr(left);
            return res;
        }
        Expr* parent = (Expr*)calloc(1, sizeof(Expr));
        parent->type = EXPR_LOGICAL;
        parent->log_op = LOGICAL_OR;
        parent->left = left;
        parent->right = right;
        left = parent;
    }
    *out = left;
    return PREPARE_SUCCESS;
}

PrepareResult parse_expr(TokenList* list, const Schema* schema, Expr** out) {
    return parse_or(list, schema, out);
}

PrepareResult parse_where_clause(TokenList* list, const Schema* schema, Statement* statement) {
    if (!match_token(list, "WHERE")) return PREPARE_SUCCESS;
    return parse_expr(list, schema, &statement->where);
}

// Parses the optional "[ORDER BY <col> [ASC|DESC]] [LIMIT <n>] [OFFSET <n>]"
// tail of a SELECT, in that order. Each clause is optional.
static PrepareResult parse_select_modifiers(TokenList* list, const Schema* schema, Statement* statement) {
    if (match_token(list, "order")) {
        if (!match_token(list, "by")) return PREPARE_SYNTAX_ERROR;
        Token col = advance_token(list);
        if (col.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
        if (schema_find_column(schema, col.text) < 0) return PREPARE_SYNTAX_ERROR;

        statement->has_order_by = true;
        strncpy(statement->order_by_column, col.text, MAX_NAME_LEN - 1);
        statement->order_by_desc = false;

        if (strcasecmp_custom(peek_token(list).text, "desc") == 0) {
            advance_token(list);
            statement->order_by_desc = true;
        } else if (strcasecmp_custom(peek_token(list).text, "asc") == 0) {
            advance_token(list);
        }
    }

    if (match_token(list, "limit")) {
        Token n = advance_token(list);
        if (n.kind != TOKEN_NUMBER) return PREPARE_SYNTAX_ERROR;
        statement->has_limit = true;
        statement->limit_count = (uint32_t)strtoul(n.text, NULL, 10);
    }

    if (match_token(list, "offset")) {
        Token n = advance_token(list);
        if (n.kind != TOKEN_NUMBER) return PREPARE_SYNTAX_ERROR;
        statement->has_offset = true;
        statement->offset_count = (uint32_t)strtoul(n.text, NULL, 10);
    }

    return PREPARE_SUCCESS;
}

// Parses the "INT" / "VARCHAR(n)" portion of a column definition, used by
// both CREATE TABLE and ALTER TABLE ADD COLUMN.
static PrepareResult parse_column_type(TokenList* list, DataType* dt_out, uint32_t* len_out) {
    Token col_type = advance_token(list);
    if (strcasecmp_custom(col_type.text, "int") == 0 || strcasecmp_custom(col_type.text, "integer") == 0) {
        *dt_out = DATA_TYPE_INT;
        *len_out = 0;
    } else if (strcasecmp_custom(col_type.text, "varchar") == 0 || strcasecmp_custom(col_type.text, "string") == 0 ||
               strcasecmp_custom(col_type.text, "char") == 0) {
        *dt_out = DATA_TYPE_VARCHAR;
        *len_out = 32;
        if (strcmp(peek_token(list).text, "(") == 0) {
            advance_token(list);
            Token len_tok = advance_token(list);
            *len_out = (uint32_t)strtoul(len_tok.text, NULL, 10);
            if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
        }
    } else {
        return PREPARE_SYNTAX_ERROR;
    }
    return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(TokenList* list, const Database* db, Statement* statement) {
    memset(statement, 0, sizeof(Statement));
    if (list->count == 0 || peek_token(list).kind == TOKEN_END) {
        return PREPARE_SYNTAX_ERROR;
    }

    Token first = peek_token(list);

    // CREATE TABLE <name> (col1 INT PRIMARY KEY, col2 VARCHAR(32), ...)
    if (strcasecmp_custom(first.text, "create") == 0) {
        advance_token(list);
        if (!match_token(list, "table")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        statement->type = STATEMENT_CREATE;
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        if (strcmp(peek_token(list).text, "(") != 0) return PREPARE_SYNTAX_ERROR;
        advance_token(list); // consume '('

        memset(&statement->created_schema, 0, sizeof(Schema));
        strncpy(statement->created_schema.table_name, tbl.text, MAX_NAME_LEN - 1);

        while (list->cursor < list->count && strcmp(peek_token(list).text, ")") != 0) {
            Token col_name = advance_token(list);
            if (col_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

            DataType dt;
            uint32_t len;
            PrepareResult type_res = parse_column_type(list, &dt, &len);
            if (type_res != PREPARE_SUCCESS) return type_res;

            bool is_pk = false;
            if (strcasecmp_custom(peek_token(list).text, "primary") == 0) {
                advance_token(list);
                if (strcasecmp_custom(peek_token(list).text, "key") == 0) advance_token(list);
                is_pk = true;
            }

            schema_add_column(&statement->created_schema, col_name.text, dt, len, is_pk);

            if (strcmp(peek_token(list).text, ",") == 0) {
                advance_token(list);
            }
        }

        if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
        return PREPARE_SUCCESS;
    }

    // DROP TABLE <name>
    if (strcasecmp_custom(first.text, "drop") == 0) {
        advance_token(list);
        if (!match_token(list, "table")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        if (tbl.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

        statement->type = STATEMENT_DROP;
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);
        return PREPARE_SUCCESS;
    }

    // ALTER TABLE <name> ADD|DROP [COLUMN] <col> [<type>] | RENAME [COLUMN <old>] TO <new>
    if (strcasecmp_custom(first.text, "alter") == 0) {
        advance_token(list);
        if (!match_token(list, "table")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        if (tbl.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

        statement->type = STATEMENT_ALTER;
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        if (match_token(list, "add")) {
            match_token(list, "column"); // optional keyword
            Token col_name = advance_token(list);
            if (col_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

            DataType dt;
            uint32_t len;
            PrepareResult type_res = parse_column_type(list, &dt, &len);
            if (type_res != PREPARE_SUCCESS) return type_res;

            statement->alter_kind = ALTER_ADD_COLUMN;
            strncpy(statement->alter_column_name, col_name.text, MAX_NAME_LEN - 1);
            statement->alter_column_type = dt;
            statement->alter_column_length = len;
            return PREPARE_SUCCESS;
        }

        if (match_token(list, "drop")) {
            match_token(list, "column"); // optional keyword
            Token col_name = advance_token(list);
            if (col_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

            statement->alter_kind = ALTER_DROP_COLUMN;
            strncpy(statement->alter_column_name, col_name.text, MAX_NAME_LEN - 1);
            return PREPARE_SUCCESS;
        }

        if (match_token(list, "rename")) {
            if (match_token(list, "column")) {
                Token old_name = advance_token(list);
                if (old_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
                if (!match_token(list, "to")) return PREPARE_SYNTAX_ERROR;
                Token new_name = advance_token(list);
                if (new_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

                statement->alter_kind = ALTER_RENAME_COLUMN;
                strncpy(statement->alter_column_name, old_name.text, MAX_NAME_LEN - 1);
                strncpy(statement->alter_new_name, new_name.text, MAX_NAME_LEN - 1);
                return PREPARE_SUCCESS;
            }
            if (!match_token(list, "to")) return PREPARE_SYNTAX_ERROR;
            Token new_name = advance_token(list);
            if (new_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

            statement->alter_kind = ALTER_RENAME_TABLE;
            strncpy(statement->alter_new_name, new_name.text, MAX_NAME_LEN - 1);
            return PREPARE_SUCCESS;
        }

        return PREPARE_SYNTAX_ERROR;
    }

    if (strcasecmp_custom(first.text, "begin") == 0 || strcasecmp_custom(first.text, "start") == 0) {
        advance_token(list);
        if (strcasecmp_custom(first.text, "start") == 0) match_token(list, "transaction");
        statement->type = STATEMENT_BEGIN;
        return PREPARE_SUCCESS;
    }

    if (strcasecmp_custom(first.text, "commit") == 0) {
        advance_token(list);
        statement->type = STATEMENT_COMMIT;
        return PREPARE_SUCCESS;
    }

    if (strcasecmp_custom(first.text, "rollback") == 0) {
        advance_token(list);
        statement->type = STATEMENT_ROLLBACK;
        return PREPARE_SUCCESS;
    }

    // INSERT INTO <table> VALUES (val1, val2, ...)
    if (strcasecmp_custom(first.text, "insert") == 0) {
        advance_token(list);
        match_token(list, "into");
        Token tbl = advance_token(list);
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        const TableCatalogEntry* entry = database_find_table(db, tbl.text);
        if (!entry) return PREPARE_NO_SUCH_TABLE;
        const Schema* schema = &entry->schema;

        if (strcmp(peek_token(list).text, "(") == 0) {
            while (list->cursor < list->count && strcmp(peek_token(list).text, ")") != 0) advance_token(list);
            if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
        }

        if (!match_token(list, "values")) return PREPARE_SYNTAX_ERROR;
        if (strcmp(peek_token(list).text, "(") == 0) advance_token(list);

        statement->type = STATEMENT_INSERT;
        statement->row_to_insert.num_values = schema->num_columns;

        for (uint32_t i = 0; i < schema->num_columns; ++i) {
            Token val_tok = advance_token(list);
            if (strcmp(peek_token(list).text, ",") == 0) advance_token(list);

            Value* v = &statement->row_to_insert.values[i];
            v->type = schema->columns[i].type;

            if (schema->columns[i].type == DATA_TYPE_INT) {
                char* endptr;
                long int_v = strtol(val_tok.text, &endptr, 10);
                if (*endptr != '\0') return PREPARE_SYNTAX_ERROR;
                if (schema->columns[i].is_primary_key && int_v < 0) return PREPARE_NEGATIVE_ID;
                v->int_val = (int32_t)int_v;
            } else {
                if (strlen(val_tok.text) > schema->columns[i].length) return PREPARE_STRING_TOO_LONG;
                strncpy(v->str_val, val_tok.text, MAX_STR_LEN - 1);
            }
        }

        if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
        return PREPARE_SUCCESS;
    }

    // SELECT [* | COUNT(*) | SUM|AVG|MIN|MAX(col)] FROM <table>
    //   [WHERE <expr>] [ORDER BY <col> [ASC|DESC]] [LIMIT n] [OFFSET n]
    if (strcasecmp_custom(first.text, "select") == 0) {
        advance_token(list);
        statement->type = STATEMENT_SELECT;
        statement->agg_type = AGG_NONE;

        Token sel = peek_token(list);
        if (strcasecmp_custom(sel.text, "count") == 0) {
            advance_token(list);
            if (strcmp(peek_token(list).text, "(") == 0) advance_token(list);
            if (strcmp(peek_token(list).text, "*") == 0) advance_token(list);
            if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
            statement->agg_type = AGG_COUNT;
        } else if (strcasecmp_custom(sel.text, "sum") == 0 || strcasecmp_custom(sel.text, "avg") == 0 ||
                   strcasecmp_custom(sel.text, "min") == 0 || strcasecmp_custom(sel.text, "max") == 0) {
            AggregateType agg = AGG_SUM;
            if (strcasecmp_custom(sel.text, "avg") == 0) agg = AGG_AVG;
            else if (strcasecmp_custom(sel.text, "min") == 0) agg = AGG_MIN;
            else if (strcasecmp_custom(sel.text, "max") == 0) agg = AGG_MAX;

            advance_token(list);
            if (!match_token(list, "(")) return PREPARE_SYNTAX_ERROR;
            Token col_tok = advance_token(list);
            if (col_tok.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
            if (!match_token(list, ")")) return PREPARE_SYNTAX_ERROR;

            statement->agg_type = agg;
            strncpy(statement->agg_column, col_tok.text, MAX_NAME_LEN - 1);
        } else if (strcmp(sel.text, "*") == 0) {
            advance_token(list);
        }

        if (!match_token(list, "from")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        const TableCatalogEntry* left_entry = database_find_table(db, tbl.text);
        if (!left_entry) return PREPARE_NO_SUCH_TABLE;

        const Schema* effective_schema = &left_entry->schema;
        Schema combined_schema; // only populated & used below if a JOIN is present

        match_token(list, "inner"); // optional, ignored -- only inner joins are supported
        if (match_token(list, "join")) {
            Token rtbl = advance_token(list);
            if (rtbl.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
            const TableCatalogEntry* right_entry = database_find_table(db, rtbl.text);
            if (!right_entry) {
                strncpy(statement->table_name, rtbl.text, MAX_NAME_LEN - 1);
                return PREPARE_NO_SUCH_TABLE;
            }

            if (!match_token(list, "on")) return PREPARE_SYNTAX_ERROR;

            Token c1 = advance_token(list);
            Token op_tok = advance_token(list);
            Token c2 = advance_token(list);
            if (c1.kind != TOKEN_IDENTIFIER || c2.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

            WhereOp join_op;
            if (strcmp(op_tok.text, "=") == 0) join_op = WHERE_OP_EQ;
            else if (strcmp(op_tok.text, "!=") == 0 || strcmp(op_tok.text, "<>") == 0) join_op = WHERE_OP_NEQ;
            else if (strcmp(op_tok.text, ">=") == 0) join_op = WHERE_OP_GE;
            else if (strcmp(op_tok.text, "<=") == 0) join_op = WHERE_OP_LE;
            else if (strcmp(op_tok.text, ">") == 0) join_op = WHERE_OP_GT;
            else if (strcmp(op_tok.text, "<") == 0) join_op = WHERE_OP_LT;
            else return PREPARE_SYNTAX_ERROR;

            // ON accepts either "left.col OP right.col" or "right.col OP left.col".
            int li = schema_find_column(&left_entry->schema, c1.text);
            int ri = schema_find_column(&right_entry->schema, c2.text);
            bool swapped = false;
            if (li < 0 || ri < 0) {
                int li2 = schema_find_column(&left_entry->schema, c2.text);
                int ri2 = schema_find_column(&right_entry->schema, c1.text);
                if (li2 < 0 || ri2 < 0) return PREPARE_SYNTAX_ERROR;
                li = li2;
                ri = ri2;
                swapped = true;
            }
            if (left_entry->schema.num_columns + right_entry->schema.num_columns > MAX_COLUMNS) {
                return PREPARE_SYNTAX_ERROR;
            }
            if (left_entry->schema.columns[li].type != right_entry->schema.columns[ri].type) {
                return PREPARE_SYNTAX_ERROR;
            }

            statement->has_join = true;
            strncpy(statement->join_table_name, rtbl.text, MAX_NAME_LEN - 1);
            strncpy(statement->join_left_column, left_entry->schema.columns[li].name, MAX_NAME_LEN - 1);
            strncpy(statement->join_right_column, right_entry->schema.columns[ri].name, MAX_NAME_LEN - 1);
            statement->join_op = swapped ? flip_op(join_op) : join_op;

            build_combined_schema(&combined_schema, &left_entry->schema, &right_entry->schema);
            effective_schema = &combined_schema;
        }

        if (statement->agg_type == AGG_SUM || statement->agg_type == AGG_AVG ||
            statement->agg_type == AGG_MIN || statement->agg_type == AGG_MAX) {
            if (schema_find_column(effective_schema, statement->agg_column) < 0) return PREPARE_SYNTAX_ERROR;
        }

        PrepareResult where_res = parse_where_clause(list, effective_schema, statement);
        if (where_res != PREPARE_SUCCESS) return where_res;

        return parse_select_modifiers(list, effective_schema, statement);
    }

    // UPDATE <table> SET col1 = val1 [, col2 = val2] [WHERE <expr>]
    if (strcasecmp_custom(first.text, "update") == 0) {
        advance_token(list);
        statement->type = STATEMENT_UPDATE;
        statement->is_set_update = true;

        Token tbl = advance_token(list); // table name
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        const TableCatalogEntry* entry = database_find_table(db, tbl.text);
        if (!entry) return PREPARE_NO_SUCH_TABLE;

        match_token(list, "set");

        while (list->cursor < list->count && strcasecmp_custom(peek_token(list).text, "where") != 0 && strcmp(peek_token(list).text, ";") != 0) {
            if (strcmp(peek_token(list).text, ",") == 0) {
                advance_token(list);
                continue;
            }

            Token col = advance_token(list);
            if (!match_token(list, "=")) return PREPARE_SYNTAX_ERROR;
            Token val = advance_token(list);

            if (statement->num_update_assignments < MAX_ASSIGNMENTS) {
                UpdateAssignment* assign = &statement->update_assignments[statement->num_update_assignments++];
                strncpy(assign->column_name, col.text, MAX_NAME_LEN - 1);
                strncpy(assign->value_text, val.text, MAX_STR_LEN - 1);
            }
        }

        return parse_where_clause(list, &entry->schema, statement);
    }

    // DELETE FROM <table> [WHERE <expr>]
    if (strcasecmp_custom(first.text, "delete") == 0) {
        advance_token(list);
        statement->type = STATEMENT_DELETE;

        if (!match_token(list, "from")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        const TableCatalogEntry* entry = database_find_table(db, tbl.text);
        if (!entry) return PREPARE_NO_SUCH_TABLE;

        return parse_where_clause(list, &entry->schema, statement);
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
}

// ============================================================================
// Execution Engine
// ============================================================================

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

// Simple insertion sort (result sets in this toy engine are small enough
// that O(n^2) is fine, and it avoids relying on non-C99 qsort_r variants).
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

static void print_aggregate(const Statement* statement, const DynamicRow* rows, uint32_t count, const Schema* schema) {
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

// Shared tail end of SELECT execution: aggregate reduction, or ORDER BY +
// LIMIT/OFFSET + printing. Used by both a plain SELECT and a JOINed one --
// `schema` describes whatever `rows` actually contains (a single table's
// row layout, or a combined JOIN row layout).
static ExecuteResult finish_select(Statement* statement, DynamicRow* rows, uint32_t count, const Schema* schema) {
    if (statement->agg_type != AGG_NONE) {
        print_aggregate(statement, rows, count, schema);
        return EXECUTE_SUCCESS;
    }

    if (statement->has_order_by) {
        int col_idx = schema_find_column(schema, statement->order_by_column);
        if (col_idx >= 0) {
            sort_rows(rows, count, (uint32_t)col_idx, schema->columns[col_idx].type, statement->order_by_desc);
        }
    }

    uint32_t start = statement->has_offset ? statement->offset_count : 0;
    if (start > count) start = count;
    uint64_t end64 = statement->has_limit ? (uint64_t)start + (uint64_t)statement->limit_count : (uint64_t)count;
    uint32_t end = (end64 > count) ? count : (uint32_t)end64;

    for (uint32_t i = start; i < end; i++) {
        print_row(&rows[i], schema);
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table) {
    Cursor* cursor = table_start(table);
    DynamicRow row;

    // Buffer matching rows first: ORDER BY, LIMIT/OFFSET, and aggregates
    // all need to see the whole result set before deciding what to print.
    uint32_t capacity = 64;
    uint32_t count = 0;
    DynamicRow* rows = (DynamicRow*)malloc(capacity * sizeof(DynamicRow));

    while (!cursor->end_of_table) {
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

    ExecuteResult result = finish_select(statement, rows, count, &table->schema);
    free(rows);
    return result;
}

// Compares one value from the left row against one from the right row for
// a JOIN's ON condition. Both sides must already be the same DataType
// (enforced at parse time).
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

// Inner-joins `left` and `right` with a simple nested loop (the right
// table is buffered once, then scanned per left row). WHERE, ORDER BY,
// LIMIT/OFFSET, and aggregates all then apply to the joined result set,
// via the combined schema built by build_combined_schema.
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

    // Buffer the right table's rows once so the nested loop below doesn't
    // re-walk its B+tree for every left row.
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
            if (!join_values_match(&lrow.values[left_col_idx], &right_rows[j].values[right_col_idx], join_type, statement->join_op)) {
                continue;
            }

            DynamicRow combined_row;
            uint32_t w = 0;
            for (uint32_t k = 0; k < lrow.num_values && w < MAX_COLUMNS; k++) combined_row.values[w++] = lrow.values[k];
            for (uint32_t k = 0; k < right_rows[j].num_values && w < MAX_COLUMNS; k++) combined_row.values[w++] = right_rows[j].values[k];
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

    // Catalog & free-list pages are ordinary pages, so the loop above
    // already restored their on-disk/cached bytes -- but db->tables[] and
    // db->free_pages[] are separate in-memory caches of them and need to
    // be refreshed to match.
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
        if (!db->tables[i].in_use) { slot = (int)i; break; }
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

    printf("CREATE TABLE %s (%u columns configured)\n", entry->schema.table_name, entry->schema.num_columns);
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

// Rebuilds a table's data under a new schema by re-inserting every row.
// dropped_col_idx is the index (into the OLD schema) of a column being
// removed, or -1 when only appending a column.
static ExecuteResult migrate_table_rows(Database* db, int idx, Schema new_schema, int dropped_col_idx) {
    TableCatalogEntry* entry = &db->tables[idx];

    uint32_t old_root = entry->root_page_num;
    Table old_view = { .pager = db->pager, .root_page_num = old_root, .schema = entry->schema, .db = db };

    uint32_t new_root = allocate_page(db);
    void* new_root_node = pager_get_page(db->pager, new_root);
    initialize_leaf_node(new_root_node);
    set_node_root(new_root_node, true);

    Table new_view = { .pager = db->pager, .root_page_num = new_root, .schema = new_schema, .db = db };

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
            // ADD COLUMN: the new column was appended as the schema's last column.
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

    // The new tree is fully built and the catalog now points at it; the
    // old tree's pages are no longer reachable, so reclaim them.
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
            printf("ALTER TABLE: renamed '%s' to '%s'\n", statement->table_name, statement->alter_new_name);
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
            printf("ALTER TABLE: renamed column '%s' to '%s'\n", statement->alter_column_name, statement->alter_new_name);
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
            schema_add_column(&new_schema, statement->alter_column_name, statement->alter_column_type,
                               statement->alter_column_length, false);
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
            Table view = { .pager = db->pager, .root_page_num = entry->root_page_num, .schema = entry->schema, .db = db };
            switch (statement->type) {
                case STATEMENT_INSERT: return execute_insert(statement, &view);
                case STATEMENT_SELECT: {
                    if (!statement->has_join) return execute_select(statement, &view);

                    const TableCatalogEntry* right_entry = database_find_table(db, statement->join_table_name);
                    if (!right_entry) {
                        printf("Error: no such table '%s'.\n", statement->join_table_name);
                        return EXECUTE_TABLE_NOT_FOUND;
                    }
                    Table right_view = { .pager = db->pager, .root_page_num = right_entry->root_page_num, .schema = right_entry->schema, .db = db };
                    return execute_select_join(statement, &view, &right_view);
                }
                case STATEMENT_UPDATE: return execute_update(statement, &view);
                case STATEMENT_DELETE: return execute_delete(statement, &view);
                default: return EXECUTE_SUCCESS;
            }
        }
    }
    return EXECUTE_SUCCESS;
}

// ============================================================================
// Shell & Meta Commands
// ============================================================================

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
        printf("  %-32s (%u columns)\n", db->tables[i].schema.table_name, db->tables[i].schema.num_columns);
    }
}

void print_free_space(const Database* db) {
    printf("File: %u page(s) (%u bytes)\n", db->pager->num_pages, db->pager->num_pages * PAGE_SIZE);
    printf("Reclaimed pages available for reuse: %u\n", db->num_free_pages);
}

// Shrinks the file by dropping trailing pages that are on the free list.
// Reclaimed pages in the middle of the file stay on the free list (and are
// still reused by future allocations) since only the very end of the file
// can be truncated.
void vacuum(Database* db) {
    // Simple insertion sort descending; num_free_pages is small (<= TABLE_MAX_PAGES).
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
            printf("Warning: VACUUM shrank the in-memory page count but the file truncate failed (errno %d).\n", errno);
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

// Splits "<command> <optional table name>" (already stripped of leading '.'
// or '\'). Returns the table name, or NULL if none was given.
static const char* meta_command_arg(const char* input) {
    const char* space = strchr(input, ' ');
    if (!space) return NULL;
    while (*space == ' ') space++;
    return (*space == '\0') ? NULL : space;
}

// Resolves the table a single-table meta command (\d, \c) should target:
// the named argument if given, else the database's only table if there is
// exactly one, else NULL (ambiguous / not found).
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
            Table view = { .pager = db->pager, .root_page_num = entry->root_page_num, .schema = entry->schema, .db = db };
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
                // Specific message already printed by the execute_* function.
                break;
        }
    }

    database_close(db);
    return 0;
}
