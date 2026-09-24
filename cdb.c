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
#define SCHEMA_MAGIC 0x5343484D

#define MAX_COLUMNS 32
#define MAX_NAME_LEN 64
#define MAX_STR_LEN 256
#define MAX_TOKENS 256
#define MAX_ASSIGNMENTS 32

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
    for (uint32_t i = 0; i < schema->num_columns; ++i) {
        if (strcasecmp_custom(schema->columns[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
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
    EXECUTE_NOT_FOUND,
    EXECUTE_TX_ALREADY_ACTIVE,
    EXECUTE_NO_ACTIVE_TX,
    EXECUTE_TABLE_FULL
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
    PREPARE_UNRECOGNIZED_STATEMENT
} PrepareResult;

typedef enum StatementType {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_UPDATE,
    STATEMENT_DELETE,
    STATEMENT_CREATE,
    STATEMENT_BEGIN,
    STATEMENT_COMMIT,
    STATEMENT_ROLLBACK
} StatementType;

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
    bool is_count;
    uint32_t target_id;

    // SET-style UPDATE
    bool is_set_update;
    UpdateAssignment update_assignments[MAX_ASSIGNMENTS];
    uint32_t num_update_assignments;
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

typedef struct Table {
    Pager* pager;
    uint32_t root_page_num;
    Schema schema;

    // Transaction state
    bool in_transaction;
    uint32_t tx_original_num_pages;
    void* tx_backup[TABLE_MAX_PAGES];
    bool tx_was_cached[TABLE_MAX_PAGES];
} Table;

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

void tx_free_backups(Table* table) {
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (table->tx_backup[i] != NULL) {
            free(table->tx_backup[i]);
            table->tx_backup[i] = NULL;
        }
    }
}

typedef struct MetaPage {
    uint32_t magic;
    uint32_t root_page_num;
    Schema schema;
} MetaPage;

Table* table_open(const char* filename) {
    Table* table = (Table*)malloc(sizeof(Table));
    table->pager = pager_open(filename);
    table->in_transaction = false;
    table->tx_original_num_pages = 0;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        table->tx_backup[i] = NULL;
        table->tx_was_cached[i] = false;
    }

    if (table->pager->num_pages == 0) {
        table->root_page_num = 1;
        memset(&table->schema, 0, sizeof(Schema));
        table->schema.has_schema = false;

        MetaPage* meta = (MetaPage*)pager_get_page(table->pager, 0);
        meta->magic = SCHEMA_MAGIC;
        meta->root_page_num = table->root_page_num;
        meta->schema = table->schema;

        void* root_node = pager_get_page(table->pager, table->root_page_num);
        initialize_leaf_node(root_node);
        set_node_root(root_node, true);
    } else {
        MetaPage* meta = (MetaPage*)pager_get_page(table->pager, 0);
        if (meta->magic == SCHEMA_MAGIC) {
            table->root_page_num = meta->root_page_num;
            table->schema = meta->schema;
        } else {
            table->root_page_num = 1;
            memset(&table->schema, 0, sizeof(Schema));
            table->schema.has_schema = false;
        }
    }
    return table;
}

void table_close(Table* table) {
    tx_free_backups(table);
    for (uint32_t i = 0; i < table->pager->num_pages; i++) {
        if (table->pager->pages[i] == NULL) continue;
        pager_flush(table->pager, i);
        free(table->pager->pages[i]);
        table->pager->pages[i] = NULL;
    }
    pager_close(table->pager);
    free(table);
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
    return table->pager->num_pages;
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

PrepareResult prepare_statement(TokenList* list, const Schema* schema, Statement* statement) {
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

            Token col_type = advance_token(list);
            DataType dt = DATA_TYPE_INT;
            uint32_t len = 0;

            if (strcasecmp_custom(col_type.text, "int") == 0 || strcasecmp_custom(col_type.text, "integer") == 0) {
                dt = DATA_TYPE_INT;
            } else if (strcasecmp_custom(col_type.text, "varchar") == 0 || strcasecmp_custom(col_type.text, "string") == 0 || strcasecmp_custom(col_type.text, "char") == 0) {
                dt = DATA_TYPE_VARCHAR;
                len = 32;
                if (strcmp(peek_token(list).text, "(") == 0) {
                    advance_token(list);
                    Token len_tok = advance_token(list);
                    len = (uint32_t)strtoul(len_tok.text, NULL, 10);
                    if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
                }
            } else {
                return PREPARE_SYNTAX_ERROR;
            }

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
        advance_token(list); // consume table name

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

    // SELECT [* | COUNT(*)] FROM <table> [WHERE <expr>]
    if (strcasecmp_custom(first.text, "select") == 0) {
        advance_token(list);
        statement->type = STATEMENT_SELECT;

        if (strcasecmp_custom(peek_token(list).text, "count") == 0) {
            advance_token(list);
            if (strcmp(peek_token(list).text, "(") == 0) advance_token(list);
            if (strcmp(peek_token(list).text, "*") == 0) advance_token(list);
            if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
            statement->is_count = true;
        } else if (strcmp(peek_token(list).text, "*") == 0) {
            advance_token(list);
        }

        if (match_token(list, "from")) {
            advance_token(list);
        }

        return parse_where_clause(list, schema, statement);
    }

    // UPDATE <table> SET col1 = val1 [, col2 = val2] [WHERE <expr>]
    if (strcasecmp_custom(first.text, "update") == 0) {
        advance_token(list);
        statement->type = STATEMENT_UPDATE;
        statement->is_set_update = true;

        advance_token(list); // table name
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

        return parse_where_clause(list, schema, statement);
    }

    // DELETE FROM <table> [WHERE <expr>]
    if (strcasecmp_custom(first.text, "delete") == 0) {
        advance_token(list);
        statement->type = STATEMENT_DELETE;

        if (match_token(list, "from")) {
            advance_token(list);
        }

        return parse_where_clause(list, schema, statement);
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

// ============================================================================
// Shell & Meta Commands
// ============================================================================

void print_help(void) {
    printf("SQL Commands:\n"
           "  CREATE TABLE <name> (<pk_col> INT PRIMARY KEY, <col2> VARCHAR(32), <col3> INT, ...);\n"
           "  INSERT INTO <name> VALUES (<val1>, '<val2>', ...);\n"
           "  SELECT * FROM <name> [WHERE <expr>];\n"
           "  SELECT COUNT(*) FROM <name> [WHERE <expr>];\n"
           "  UPDATE <name> SET <col> = <val> WHERE <expr>;\n"
           "  DELETE FROM <name> WHERE <expr>;\n"
           "  (WHERE supports =, !=, <>, <, >, <=, >=, AND, OR, and parentheses ())\n"
           "  BEGIN; | COMMIT; | ROLLBACK;\n"
           "Meta commands:\n"
           "  \\q or .exit      quit the shell\n"
           "  \\d or .btree     print the B+tree structure\n"
           "  \\c or .constants print page size constants\n"
           "  \\? or .help      show this message\n");
}

void print_constants(const Table* table) {
    printf("ROW_SIZE: %u\n"
           "COMMON_NODE_HEADER_SIZE: %zu\n"
           "LEAF_NODE_HEADER_SIZE: %zu\n"
           "LEAF_NODE_CELL_SIZE: %u\n"
           "LEAF_NODE_MAX_CELLS: %u\n",
           table->schema.row_size,
           (size_t)COMMON_NODE_HEADER_SIZE,
           (size_t)LEAF_NODE_HEADER_SIZE,
           leaf_node_cell_size(table->schema.row_size),
           leaf_node_max_cells(table->schema.row_size));
}

MetaCommandResult do_meta_command(const char* input, Table* table) {
    if (strcmp(input, ".exit") == 0 || strcmp(input, "\\q") == 0) {
        table_close(table);
        exit(EXIT_SUCCESS);
    } else if (strcmp(input, ".btree") == 0 || strcmp(input, "\\d") == 0) {
        printf("Tree:\n");
        print_tree(table, table->root_page_num, 0);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input, ".constants") == 0 || strcmp(input, "\\c") == 0) {
        printf("Constants:\n");
        print_constants(table);
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
    Table* table = table_open(filename);

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
            switch (do_meta_command(input_buffer, table)) {
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
        PrepareResult prep_res = prepare_statement(&tokens, &table->schema, &statement);

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
        }

        if (!table->schema.has_schema &&
            (statement.type == STATEMENT_INSERT || statement.type == STATEMENT_SELECT ||
             statement.type == STATEMENT_UPDATE || statement.type == STATEMENT_DELETE)) {
            printf("Error: No table schema found. Please run CREATE TABLE first.\n");
            free_expr(statement.where);
            continue;
        }

        ExecuteResult exec_res = execute_statement(&statement, table);
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
            case EXECUTE_NOT_FOUND:
                printf("Error: row not found.\n");
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
        }
    }

    table_close(table);
    return 0;
}
