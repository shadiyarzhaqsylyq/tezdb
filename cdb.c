//#define _POSIX_C_SOURCE 200809L // for pread/pwrite/ftruncate declarations
//#define _GNU_SOURCE
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

#define PAGE_SIZE 4096
#define TABLE_MAX_PAGES 400
#define INVALID_PAGE_NUM UINT32_MAX
#define INSERT_PAGE_SAFETY_MARGIN 16
#define SCHEMA_MAGIC 0x54455A44 /* "TEZD" */

#define MAX_COLUMNS 16
#define MAX_NAME_LEN 32
#define MAX_STR_LEN 255
#define MAX_TOKENS 128
#define MAX_ASSIGNMENTS 16

/* ==================================================================== */
/* Data Types & Schema                                                  */
/* ==================================================================== */

typedef enum {
    TYPE_INT,
    TYPE_STRING
} DataType;

typedef struct {
    char name[MAX_NAME_LEN];
    DataType type;
    uint32_t size;
    uint32_t offset;
    bool is_pk;
} ColumnDef;

typedef struct {
    uint32_t magic;
    uint32_t root_page_num;
    uint32_t num_columns;
    uint32_t row_size;
    int32_t pk_col_idx;
    ColumnDef columns[MAX_COLUMNS];
} Schema;

typedef struct {
    DataType type;
    union {
        int32_t int_val;
        char string_val[MAX_STR_LEN + 1];
    };
} Value;

typedef struct {
    Value values[MAX_COLUMNS];
} DynamicRow;

static int strcasecmp_custom(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        int diff = tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
        if (diff != 0) return diff;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

static int schema_find_column(const Schema* schema, const char* name) {
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (strcasecmp_custom(schema->columns[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool schema_add_column(Schema* schema, const char* name, DataType type, uint32_t str_len, bool is_pk) {
    if (schema->num_columns >= MAX_COLUMNS) return false;
    if (schema_find_column(schema, name) != -1) return false;

    ColumnDef* col = &schema->columns[schema->num_columns];
    strncpy(col->name, name, MAX_NAME_LEN - 1);
    col->name[MAX_NAME_LEN - 1] = '\0';
    col->type = type;
    col->is_pk = is_pk;

    if (type == TYPE_INT) {
        col->size = sizeof(int32_t);
    } else {
        col->size = (str_len > MAX_STR_LEN ? MAX_STR_LEN : str_len) + 1;
    }

    col->offset = schema->row_size;
    schema->row_size += col->size;

    if (is_pk) {
        schema->pk_col_idx = (int32_t)schema->num_columns;
    }

    schema->num_columns++;
    return true;
}

static uint32_t get_pk_value(const Schema* schema, const DynamicRow* row) {
    if (schema->pk_col_idx >= 0 && schema->pk_col_idx < (int32_t)schema->num_columns) {
        if (schema->columns[schema->pk_col_idx].type == TYPE_INT) {
            return (uint32_t)row->values[schema->pk_col_idx].int_val;
        }
    }
    return (uint32_t)row->values[0].int_val;
}

static void serialize_row(const Schema* schema, const DynamicRow* source, void* destination) {
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        const ColumnDef* col = &schema->columns[i];
        char* dest = (char*)destination + col->offset;
        if (col->type == TYPE_INT) {
            memcpy(dest, &source->values[i].int_val, sizeof(int32_t));
        } else {
            memset(dest, 0, col->size);
            strncpy(dest, source->values[i].string_val, col->size - 1);
        }
    }
}

static void deserialize_row(const Schema* schema, const void* source, DynamicRow* destination) {
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        const ColumnDef* col = &schema->columns[i];
        const char* src = (const char*)source + col->offset;
        destination->values[i].type = col->type;
        if (col->type == TYPE_INT) {
            memcpy(&destination->values[i].int_val, src, sizeof(int32_t));
        } else {
            memset(destination->values[i].string_val, 0, sizeof(destination->values[i].string_val));
            strncpy(destination->values[i].string_val, src, col->size - 1);
        }
    }
}

static void print_row(const Schema* schema, const DynamicRow* row, const int* selected_cols, int num_selected) {
    printf("(");
    int count = (num_selected > 0) ? num_selected : (int)schema->num_columns;
    for (int idx = 0; idx < count; idx++) {
        int i = (num_selected > 0) ? selected_cols[idx] : idx;
        if (schema->columns[i].type == TYPE_INT) {
            printf("%d", row->values[i].int_val);
        } else {
            printf("%s", row->values[i].string_val);
        }
        if (idx < count - 1) printf(", ");
    }
    printf(")\n");
}

/* ==================================================================== */
/* AST & WHERE Expressions                                              */
/* ==================================================================== */

typedef enum {
    OP_EQUALS,
    OP_NOT_EQUALS,
    OP_GREATER,
    OP_LESS,
    OP_GREATER_EQUALS,
    OP_LESS_EQUALS
} WhereOp;

typedef enum {
    LOGICAL_NONE,
    LOGICAL_AND,
    LOGICAL_OR
} LogicalOp;

typedef enum {
    EXPR_BINARY_OP,
    EXPR_LOGICAL_OP
} ExprType;

typedef struct Expr {
    ExprType type;
    union {
        struct {
            int col_idx;
            WhereOp op;
            Value literal;
        } binary;
        struct {
            LogicalOp op;
            struct Expr* left;
            struct Expr* right;
        } logical;
    };
} Expr;

static void free_expr(Expr* expr) {
    if (!expr) return;
    if (expr->type == EXPR_LOGICAL_OP) {
        free_expr(expr->logical.left);
        free_expr(expr->logical.right);
    }
    free(expr);
}

static bool evaluate_expr(const Expr* expr, const Schema* schema, const DynamicRow* row) {
    if (!expr) return true;

    if (expr->type == EXPR_LOGICAL_OP) {
        bool left_val = evaluate_expr(expr->logical.left, schema, row);
        if (expr->logical.op == LOGICAL_AND) {
            if (!left_val) return false;
            return evaluate_expr(expr->logical.right, schema, row);
        } else if (expr->logical.op == LOGICAL_OR) {
            if (left_val) return true;
            return evaluate_expr(expr->logical.right, schema, row);
        }
        return false;
    }

    int col = expr->binary.col_idx;
    if (col < 0 || col >= (int)schema->num_columns) return false;

    if (schema->columns[col].type == TYPE_INT) {
        int32_t actual = row->values[col].int_val;
        int32_t target = expr->binary.literal.int_val;
        switch (expr->binary.op) {
            case OP_EQUALS:         return actual == target;
            case OP_NOT_EQUALS:     return actual != target;
            case OP_GREATER:        return actual > target;
            case OP_LESS:           return actual < target;
            case OP_GREATER_EQUALS: return actual >= target;
            case OP_LESS_EQUALS:    return actual <= target;
        }
    } else {
        int cmp = strcmp(row->values[col].string_val, expr->binary.literal.string_val);
        switch (expr->binary.op) {
            case OP_EQUALS:         return cmp == 0;
            case OP_NOT_EQUALS:     return cmp != 0;
            case OP_GREATER:        return cmp > 0;
            case OP_LESS:           return cmp < 0;
            case OP_GREATER_EQUALS: return cmp >= 0;
            case OP_LESS_EQUALS:    return cmp <= 0;
        }
    }
    return false;
}

/* ==================================================================== */
/* Storage Engine & Pager                                               */
/* ==================================================================== */

typedef struct {
    int file_descriptor;
    uint32_t file_length;
    uint32_t num_pages;
    void* pages[TABLE_MAX_PAGES];
} Pager;

typedef struct {
    Pager* pager;
    Schema schema;
    uint32_t root_page_num;
} Table;

static Pager* pager_open(const char* filename) {
    int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    if (fd == -1) {
        perror("Unable to open file");
        exit(EXIT_FAILURE);
    }

    off_t file_length = lseek(fd, 0, SEEK_END);
    Pager* pager = malloc(sizeof(Pager));
    pager->file_descriptor = fd;
    pager->file_length = (uint32_t)file_length;
    pager->num_pages = (uint32_t)(file_length / PAGE_SIZE);

    if (file_length % PAGE_SIZE != 0) {
        printf("Corrupt database file (partial page found).\n");
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->pages[i] = NULL;
    }
    return pager;
}

static void* pager_get_page(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) {
        printf("Page number %u exceeds maximum pages %d.\n", page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    if (pager->pages[page_num] == NULL) {
        void* page = calloc(1, PAGE_SIZE);
        uint32_t num_pages = pager->file_length / PAGE_SIZE;

        if (pager->file_length % PAGE_SIZE) {
            num_pages += 1;
        }

        if (page_num < num_pages) {
            lseek(pager->file_descriptor, (off_t)page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
            if (bytes_read == -1) {
                perror("Error reading page from disk");
                free(page);
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

static void pager_flush(Pager* pager, uint32_t page_num) {
    if (pager->pages[page_num] == NULL) return;

    off_t offset = lseek(pager->file_descriptor, (off_t)page_num * PAGE_SIZE, SEEK_SET);
    if (offset == -1) {
        perror("Error seeking during flush");
        exit(EXIT_FAILURE);
    }

    size_t total_written = 0;
    while (total_written < PAGE_SIZE) {
        ssize_t bytes_written = write(
            pager->file_descriptor,
            (char*)pager->pages[page_num] + total_written,
            PAGE_SIZE - total_written
        );
        if (bytes_written <= 0) {
            perror("Error writing page to disk");
            exit(EXIT_FAILURE);
        }
        total_written += bytes_written;
    }
}

static void pager_close(Pager* pager) {
    for (uint32_t i = 0; i < pager->num_pages; i++) {
        if (pager->pages[i] != NULL) {
            pager_flush(pager, i);
            free(pager->pages[i]);
            pager->pages[i] = NULL;
        }
    }

    int result = close(pager->file_descriptor);
    if (result == -1) {
        perror("Error closing db file");
        exit(EXIT_FAILURE);
    }
    free(pager);
}

/* ==================================================================== */
/* B+ Tree Page Layout Constants                                        */
/* ==================================================================== */

typedef enum {
    NODE_INTERNAL = 0,
    NODE_LEAF = 1
} NodeType;

#define NODE_TYPE_SIZE           sizeof(uint8_t)
#define NODE_TYPE_OFFSET         0
#define IS_ROOT_SIZE             sizeof(uint8_t)
#define IS_ROOT_OFFSET           NODE_TYPE_SIZE
#define PARENT_POINTER_SIZE      sizeof(uint32_t)
#define PARENT_POINTER_OFFSET    (IS_ROOT_OFFSET + IS_ROOT_SIZE)
#define COMMON_NODE_HEADER_SIZE  (NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE)

/* Internal Node Layout */
#define INTERNAL_NODE_NUM_KEYS_SIZE      sizeof(uint32_t)
#define INTERNAL_NODE_NUM_KEYS_OFFSET    COMMON_NODE_HEADER_SIZE
#define INTERNAL_NODE_RIGHT_CHILD_SIZE   sizeof(uint32_t)
#define INTERNAL_NODE_RIGHT_CHILD_OFFSET (INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE)
#define INTERNAL_NODE_HEADER_SIZE        (COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE + INTERNAL_NODE_RIGHT_CHILD_SIZE)

#define INTERNAL_NODE_KEY_SIZE           sizeof(uint32_t)
#define INTERNAL_NODE_CHILD_SIZE         sizeof(uint32_t)
#define INTERNAL_NODE_CELL_SIZE          (INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE)
#define INTERNAL_NODE_MAX_KEYS           3

/* Leaf Node Layout */
#define LEAF_NODE_NUM_CELLS_SIZE         sizeof(uint32_t)
#define LEAF_NODE_NUM_CELLS_OFFSET       COMMON_NODE_HEADER_SIZE
#define LEAF_NODE_NEXT_LEAF_SIZE         sizeof(uint32_t)
#define LEAF_NODE_NEXT_LEAF_OFFSET       (LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE)
#define LEAF_NODE_HEADER_SIZE            (COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE + LEAF_NODE_NEXT_LEAF_SIZE)
#define LEAF_NODE_KEY_SIZE               sizeof(uint32_t)

static NodeType get_node_type(void* node) {
    uint8_t val = *((uint8_t*)((char*)node + NODE_TYPE_OFFSET));
    return (NodeType)val;
}

static void set_node_type(void* node, NodeType type) {
    *((uint8_t*)((char*)node + NODE_TYPE_OFFSET)) = (uint8_t)type;
}

static bool is_node_root(void* node) {
    uint8_t val = *((uint8_t*)((char*)node + IS_ROOT_OFFSET));
    return (bool)val;
}

static void set_node_root(void* node, bool is_root) {
    *((uint8_t*)((char*)node + IS_ROOT_OFFSET)) = (uint8_t)is_root;
}

static uint32_t* node_parent(void* node) {
    return (uint32_t*)((char*)node + PARENT_POINTER_OFFSET);
}

static uint32_t* internal_node_num_keys(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_NUM_KEYS_OFFSET);
}

static uint32_t* internal_node_right_child(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

static uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_HEADER_SIZE + cell_num * INTERNAL_NODE_CELL_SIZE);
}

static uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) {
        printf("Tried to access child_num %u > num_keys %u\n", child_num, num_keys);
        exit(EXIT_FAILURE);
    }
    if (child_num == num_keys) {
        return internal_node_right_child(node);
    }
    return internal_node_cell(node, child_num);
}

static uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return (uint32_t*)((char*)internal_node_cell(node, key_num) + INTERNAL_NODE_CHILD_SIZE);
}

static uint32_t leaf_node_cell_size(const Schema* schema) {
    return LEAF_NODE_KEY_SIZE + schema->row_size;
}

static uint32_t leaf_node_max_cells(const Schema* schema) {
    return (PAGE_SIZE - LEAF_NODE_HEADER_SIZE) / leaf_node_cell_size(schema);
}

static uint32_t* leaf_node_num_cells(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NUM_CELLS_OFFSET);
}

static uint32_t* leaf_node_next_leaf(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NEXT_LEAF_OFFSET);
}

static void* leaf_node_cell(void* node, uint32_t cell_num, uint32_t cell_size) {
    return (char*)node + LEAF_NODE_HEADER_SIZE + cell_num * cell_size;
}

static uint32_t* leaf_node_key(void* node, uint32_t cell_num, uint32_t cell_size) {
    return (uint32_t*)leaf_node_cell(node, cell_num, cell_size);
}

static void* leaf_node_value(void* node, uint32_t cell_num, uint32_t cell_size) {
    return (char*)leaf_node_cell(node, cell_num, cell_size) + LEAF_NODE_KEY_SIZE;
}

static uint32_t get_node_max_key(Table* table, void* node) {
    if (get_node_type(node) == NODE_INTERNAL) {
        uint32_t right_child = *internal_node_right_child(node);
        return get_node_max_key(table, pager_get_page(table->pager, right_child));
    }
    uint32_t cell_size = leaf_node_cell_size(&table->schema);
    uint32_t num_cells = *leaf_node_num_cells(node);
    if (num_cells == 0) return 0;
    return *leaf_node_key(node, num_cells - 1, cell_size);
}

static void initialize_leaf_node(void* node) {
    set_node_type(node, NODE_LEAF);
    set_node_root(node, false);
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = INVALID_PAGE_NUM;
    *node_parent(node) = INVALID_PAGE_NUM;
}

static void initialize_internal_node(void* node) {
    set_node_type(node, NODE_INTERNAL);
    set_node_root(node, false);
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = INVALID_PAGE_NUM;
    *node_parent(node) = INVALID_PAGE_NUM;
}

/* ==================================================================== */
/* Cursor Abstraction & Tree Search                                     */
/* ==================================================================== */

typedef struct {
    Table* table;
    uint32_t page_num;
    uint32_t cell_num;
    bool end_of_table;
} Cursor;

static Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void* node = pager_get_page(table->pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    uint32_t cell_size = leaf_node_cell_size(&table->schema);

    Cursor* cursor = malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->page_num = page_num;
    cursor->end_of_table = false;

    /* Binary search inside leaf */
    uint32_t min_idx = 0;
    uint32_t one_past_max_idx = num_cells;
    while (one_past_max_idx != min_idx) {
        uint32_t idx = (min_idx + one_past_max_idx) / 2;
        uint32_t key_at_idx = *leaf_node_key(node, idx, cell_size);
        if (key == key_at_idx) {
            cursor->cell_num = idx;
            return cursor;
        }
        if (key < key_at_idx) {
            one_past_max_idx = idx;
        } else {
            min_idx = idx + 1;
        }
    }

    cursor->cell_num = min_idx;
    return cursor;
}

static uint32_t internal_node_find_child(void* node, uint32_t key) {
    uint32_t num_keys = *internal_node_num_keys(node);
    uint32_t min_idx = 0;
    uint32_t max_idx = num_keys;

    while (min_idx != max_idx) {
        uint32_t idx = (min_idx + max_idx) / 2;
        uint32_t key_to_right = *internal_node_key(node, idx);
        if (key_to_right >= key) {
            max_idx = idx;
        } else {
            min_idx = idx + 1;
        }
    }
    return *internal_node_child(node, min_idx);
}

static Cursor* tree_find(Table* table, uint32_t page_num, uint32_t key) {
    void* node = pager_get_page(table->pager, page_num);
    if (get_node_type(node) == NODE_LEAF) {
        return leaf_node_find(table, page_num, key);
    } else {
        uint32_t child_num = internal_node_find_child(node, key);
        return tree_find(table, child_num, key);
    }
}

static Cursor* table_start(Table* table) {
    Cursor* cursor = tree_find(table, table->root_page_num, 0);
    void* node = pager_get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    cursor->end_of_table = (num_cells == 0);
    return cursor;
}

static void cursor_advance(Cursor* cursor) {
    uint32_t page_num = cursor->page_num;
    void* node = pager_get_page(cursor->table->pager, page_num);
    cursor->cell_num += 1;

    if (cursor->cell_num >= *leaf_node_num_cells(node)) {
        uint32_t next_page_num = *leaf_node_next_leaf(node);
        if (next_page_num == INVALID_PAGE_NUM) {
            cursor->end_of_table = true;
        } else {
            cursor->page_num = next_page_num;
            cursor->cell_num = 0;
        }
    }
}

static void* cursor_value(Cursor* cursor) {
    void* page = pager_get_page(cursor->table->pager, cursor->page_num);
    uint32_t cell_size = leaf_node_cell_size(&cursor->table->schema);
    return leaf_node_value(page, cursor->cell_num, cell_size);
}

/* ==================================================================== */
/* B+ Tree Splitting & Insertion                                        */
/* ==================================================================== */

static void internal_node_insert(Table* table, uint32_t parent_page_num, uint32_t child_page_num);

static uint32_t get_unused_page_num(Table* table) {
    /* Page 0 is reserved for Schema */
    uint32_t page_num = table->pager->num_pages;
    if (page_num == 0) page_num = 1;
    table->pager->num_pages = page_num + 1;
    return page_num;
}

static void create_new_root(Table* table, uint32_t right_child_page_num) {
    void* root = pager_get_page(table->pager, table->root_page_num);
    void* right_child = pager_get_page(table->pager, right_child_page_num);
    uint32_t left_child_page_num = get_unused_page_num(table);
    void* left_child = pager_get_page(table->pager, left_child_page_num);

    /* Move existing root contents into left_child */
    memcpy(left_child, root, PAGE_SIZE);
    set_node_root(left_child, false);
    *node_parent(left_child) = table->root_page_num;

    if (get_node_type(left_child) == NODE_INTERNAL) {
        for (uint32_t i = 0; i <= *internal_node_num_keys(left_child); i++) {
            void* ch = pager_get_page(table->pager, *internal_node_child(left_child, i));
            *node_parent(ch) = left_child_page_num;
        }
    }

    *node_parent(right_child) = table->root_page_num;

    /* Initialize the new root internal page */
    initialize_internal_node(root);
    set_node_root(root, true);
    *internal_node_num_keys(root) = 1;
    *internal_node_child(root, 0) = left_child_page_num;
    *internal_node_key(root, 0) = get_node_max_key(table, left_child);
    *internal_node_right_child(root) = right_child_page_num;
}

static void internal_node_split_and_insert(Table* table, uint32_t parent_page_num, uint32_t child_page_num) {
    uint32_t old_page_num = parent_page_num;
    void* old_node = pager_get_page(table->pager, old_page_num);
    uint32_t old_max = get_node_max_key(table, old_node);

    void* child = pager_get_page(table->pager, child_page_num);
    uint32_t child_max = get_node_max_key(table, child);

    uint32_t new_page_num = get_unused_page_num(table);
    void* new_node = pager_get_page(table->pager, new_page_num);
    initialize_internal_node(new_node);

    *node_parent(new_node) = *node_parent(old_node);

    /* Temporary storage for split */
    uint32_t total_keys = INTERNAL_NODE_MAX_KEYS + 1;
    uint32_t temp_keys[INTERNAL_NODE_MAX_KEYS + 1];
    uint32_t temp_children[INTERNAL_NODE_MAX_KEYS + 2];

    uint32_t cur_keys = *internal_node_num_keys(old_node);
    uint32_t insert_idx = 0;
    while (insert_idx < cur_keys && *internal_node_key(old_node, insert_idx) < child_max) {
        insert_idx++;
    }

    uint32_t src = 0;
    for (uint32_t i = 0; i < total_keys; i++) {
        if (i == insert_idx) {
            temp_keys[i] = child_max;
            temp_children[i] = child_page_num;
        } else {
            temp_keys[i] = *internal_node_key(old_node, src);
            temp_children[i] = *internal_node_child(old_node, src);
            src++;
        }
    }
    temp_children[total_keys] = *internal_node_right_child(old_node);

    /* Divide evenly */
    uint32_t split_idx = total_keys / 2;

    *internal_node_num_keys(old_node) = split_idx;
    for (uint32_t i = 0; i < split_idx; i++) {
        *internal_node_key(old_node, i) = temp_keys[i];
        *internal_node_child(old_node, i) = temp_children[i];
    }
    *internal_node_right_child(old_node) = temp_children[split_idx];

    *internal_node_num_keys(new_node) = total_keys - split_idx - 1;
    for (uint32_t i = split_idx + 1; i < total_keys; i++) {
        uint32_t dest = i - (split_idx + 1);
        *internal_node_key(new_node, dest) = temp_keys[i];
        *internal_node_child(new_node, dest) = temp_children[i];
    }
    *internal_node_right_child(new_node) = temp_children[total_keys];

    /* Update child parent pointers */
    for (uint32_t i = 0; i <= *internal_node_num_keys(new_node); i++) {
        void* ch = pager_get_page(table->pager, *internal_node_child(new_node, i));
        *node_parent(ch) = new_page_num;
    }

    if (is_node_root(old_node)) {
        create_new_root(table, new_page_num);
    } else {
        uint32_t parent = *node_parent(old_node);
        uint32_t new_max = get_node_max_key(table, old_node);

        /* Update key in parent */
        void* pnode = pager_get_page(table->pager, parent);
        for (uint32_t i = 0; i < *internal_node_num_keys(pnode); i++) {
            if (*internal_node_key(pnode, i) == old_max) {
                *internal_node_key(pnode, i) = new_max;
                break;
            }
        }
        internal_node_insert(table, parent, new_page_num);
    }
}

static void internal_node_insert(Table* table, uint32_t parent_page_num, uint32_t child_page_num) {
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
    void* right_child = pager_get_page(table->pager, right_child_page_num);

    if (child_max_key > get_node_max_key(table, right_child)) {
        *internal_node_child(parent, original_num_keys) = right_child_page_num;
        *internal_node_key(parent, original_num_keys) = get_node_max_key(table, right_child);
        *internal_node_right_child(parent) = child_page_num;
    } else {
        for (uint32_t i = original_num_keys; i > index; i--) {
            void* dest = internal_node_cell(parent, i);
            void* src = internal_node_cell(parent, i - 1);
            memcpy(dest, src, INTERNAL_NODE_CELL_SIZE);
        }
        *internal_node_child(parent, index) = child_page_num;
        *internal_node_key(parent, index) = child_max_key;
    }
    *internal_node_num_keys(parent) += 1;
    *node_parent(child) = parent_page_num;
}

static void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, const void* value) {
    Table* table = cursor->table;
    uint32_t cell_size = leaf_node_cell_size(&table->schema);
    void* old_node = pager_get_page(table->pager, cursor->page_num);
    uint32_t old_max = get_node_max_key(table, old_node);

    uint32_t new_page_num = get_unused_page_num(table);
    void* new_node = pager_get_page(table->pager, new_page_num);
    initialize_leaf_node(new_node);

    *node_parent(new_node) = *node_parent(old_node);
    *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(old_node) = new_page_num;

    uint32_t max_cells = leaf_node_max_cells(&table->schema);
    for (int32_t i = (int32_t)max_cells; i >= 0; i--) {
        void* destination_node;
        if (i >= (int32_t)(max_cells + 1) / 2) {
            destination_node = new_node;
        } else {
            destination_node = old_node;
        }
        uint32_t index_within_node = (uint32_t)i % ((max_cells + 1) / 2);
        void* destination = leaf_node_cell(destination_node, index_within_node, cell_size);

        if ((uint32_t)i == cursor->cell_num) {
            *(uint32_t*)destination = key;
            memcpy((char*)destination + LEAF_NODE_KEY_SIZE, value, table->schema.row_size);
        } else if ((uint32_t)i > cursor->cell_num) {
            memcpy(destination, leaf_node_cell(old_node, i - 1, cell_size), cell_size);
        } else {
            memcpy(destination, leaf_node_cell(old_node, i, cell_size), cell_size);
        }
    }

    *leaf_node_num_cells(old_node) = (max_cells + 1) / 2;
    *leaf_node_num_cells(new_node) = (max_cells + 1) - *leaf_node_num_cells(old_node);

    if (is_node_root(old_node)) {
        create_new_root(table, new_page_num);
    } else {
        uint32_t parent_page_num = *node_parent(old_node);
        uint32_t new_max = get_node_max_key(table, old_node);

        void* parent = pager_get_page(table->pager, parent_page_num);
        for (uint32_t i = 0; i < *internal_node_num_keys(parent); i++) {
            if (*internal_node_key(parent, i) == old_max) {
                *internal_node_key(parent, i) = new_max;
                break;
            }
        }
        internal_node_insert(table, parent_page_num, new_page_num);
    }
}

typedef enum {
    EXECUTE_SUCCESS,
    EXECUTE_TABLE_FULL,
    EXECUTE_DUPLICATE_KEY,
    EXECUTE_ERROR
} ExecuteResult;

static ExecuteResult leaf_node_insert(Cursor* cursor, uint32_t key, const void* value) {
    Table* table = cursor->table;
    uint32_t cell_size = leaf_node_cell_size(&table->schema);
    void* node = pager_get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    /* Duplicate Key Check */
    if (cursor->cell_num < num_cells) {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num, cell_size);
        if (key_at_index == key) {
            return EXECUTE_DUPLICATE_KEY;
        }
    }

    if (num_cells >= leaf_node_max_cells(&table->schema)) {
        leaf_node_split_and_insert(cursor, key, value);
        return EXECUTE_SUCCESS;
    }

    if (cursor->cell_num < num_cells) {
        for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
            memcpy(leaf_node_cell(node, i, cell_size),
                   leaf_node_cell(node, i - 1, cell_size),
                   cell_size);
        }
    }

    *(leaf_node_num_cells(node)) += 1;
    *(leaf_node_key(node, cursor->cell_num, cell_size)) = key;
    memcpy(leaf_node_value(node, cursor->cell_num, cell_size), value, table->schema.row_size);
    return EXECUTE_SUCCESS;
}

/* ==================================================================== */
/* Database / Table Lifecycle & Page 0 Metadata                         */
/* ==================================================================== */

static Table* db_open(const char* filename) {
    Pager* pager = pager_open(filename);
    Table* table = malloc(sizeof(Table));
    table->pager = pager;

    if (pager->num_pages > 0) {
        /* Read Schema metadata from Page 0 */
        void* page0 = pager_get_page(pager, 0);
        memcpy(&table->schema, page0, sizeof(Schema));
        if (table->schema.magic == SCHEMA_MAGIC) {
            table->root_page_num = table->schema.root_page_num;
            return table;
        }
    }

    /* Create New Table Schema */
    memset(&table->schema, 0, sizeof(Schema));
    table->schema.magic = SCHEMA_MAGIC;
    table->schema.root_page_num = 1;
    table->schema.pk_col_idx = 0;

    schema_add_column(&table->schema, "id", TYPE_INT, 0, true);
    schema_add_column(&table->schema, "username", TYPE_STRING, 32, false);
    schema_add_column(&table->schema, "email", TYPE_STRING, 64, false);

    table->root_page_num = 1;

    /* Initialize Root Page 1 */
    void* root_node = pager_get_page(pager, 1);
    initialize_leaf_node(root_node);
    set_node_root(root_node, true);

    /* Persist Page 0 */
    void* page0 = pager_get_page(pager, 0);
    table->schema.root_page_num = table->root_page_num;
    memcpy(page0, &table->schema, sizeof(Schema));
    pager_flush(pager, 0);
    pager_flush(pager, 1);

    return table;
}

static void db_close(Table* table) {
    /* Update root page in schema header before closing */
    table->schema.root_page_num = table->root_page_num;
    void* page0 = pager_get_page(table->pager, 0);
    memcpy(page0, &table->schema, sizeof(Schema));
    pager_flush(table->pager, 0);

    pager_close(table->pager);
    free(table);
}

/* ==================================================================== */
/* SQL Parsing & Statements                                             */
/* ==================================================================== */

typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
    PREPARE_SUCCESS,
    PREPARE_SYNTAX_ERROR,
    PREPARE_STRING_TOO_LONG,
    PREPARE_UNRECOGNIZED_STATEMENT
} PrepareResult;

typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_UPDATE,
    STATEMENT_DELETE
} StatementType;

typedef struct {
    int col_idx;
    Value value;
} UpdateAssignment;

typedef struct {
    StatementType type;
    DynamicRow row_to_insert;
    Expr* where_clause;
    UpdateAssignment assignments[MAX_ASSIGNMENTS];
    int num_assignments;
    int selected_cols[MAX_COLUMNS];
    int num_selected_cols;
} Statement;

static void clean_token(char* dest, const char* src, size_t max_dest) {
    size_t i = 0;
    while (*src && isspace((unsigned char)*src)) src++;
    bool in_quotes = (*src == '\'' || *src == '"');
    char quote_char = in_quotes ? *src++ : '\0';

    while (*src && i < max_dest - 1) {
        if (in_quotes && *src == quote_char) break;
        if (!in_quotes && (isspace((unsigned char)*src) || *src == ',' || *src == ';')) break;
        dest[i++] = *src++;
    }
    dest[i] = '\0';
}

static Expr* parse_comparison(const Schema* schema, const char* left_tok, const char* op_tok, const char* right_tok) {
    int col = schema_find_column(schema, left_tok);
    if (col == -1) return NULL;

    WhereOp op;
    if (strcmp(op_tok, "=") == 0 || strcmp(op_tok, "==") == 0) {
        op = OP_EQUALS;
    } else if (strcmp(op_tok, "!=") == 0 || strcmp(op_tok, "<>") == 0) {
        op = OP_NOT_EQUALS;
    } else if (strcmp(op_tok, ">") == 0) {
        op = OP_GREATER;
    } else if (strcmp(op_tok, "<") == 0) {
        op = OP_LESS;
    } else if (strcmp(op_tok, ">=") == 0) {
        op = OP_GREATER_EQUALS;
    } else if (strcmp(op_tok, "<=") == 0) {
        op = OP_LESS_EQUALS;
    } else {
        return NULL;
    }

    Expr* expr = calloc(1, sizeof(Expr));
    expr->type = EXPR_BINARY_OP;
    expr->binary.col_idx = col;
    expr->binary.op = op;

    if (schema->columns[col].type == TYPE_INT) {
        expr->binary.literal.type = TYPE_INT;
        expr->binary.literal.int_val = atoi(right_tok);
    } else {
        expr->binary.literal.type = TYPE_STRING;
        strncpy(expr->binary.literal.string_val, right_tok, MAX_STR_LEN);
    }
    return expr;
}

static Expr* parse_where_clause(const Schema* schema, char* where_str) {
    if (!where_str) return NULL;

    char* and_pos = strstr(where_str, " AND ");
    char* and_pos_lower = strstr(where_str, " and ");
    char* target_and = and_pos ? and_pos : and_pos_lower;

    if (target_and) {
        *target_and = '\0';
        char* left_part = where_str;
        char* right_part = target_and + 5;

        Expr* left_expr = parse_where_clause(schema, left_part);
        Expr* right_expr = parse_where_clause(schema, right_part);
        if (!left_expr || !right_expr) {
            free_expr(left_expr);
            free_expr(right_expr);
            return NULL;
        }

        Expr* expr = calloc(1, sizeof(Expr));
        expr->type = EXPR_LOGICAL_OP;
        expr->logical.op = LOGICAL_AND;
        expr->logical.left = left_expr;
        expr->logical.right = right_expr;
        return expr;
    }

    char* or_pos = strstr(where_str, " OR ");
    char* or_pos_lower = strstr(where_str, " or ");
    char* target_or = or_pos ? or_pos : or_pos_lower;

    if (target_or) {
        *target_or = '\0';
        char* left_part = where_str;
        char* right_part = target_or + 4;

        Expr* left_expr = parse_where_clause(schema, left_part);
        Expr* right_expr = parse_where_clause(schema, right_part);
        if (!left_expr || !right_expr) {
            free_expr(left_expr);
            free_expr(right_expr);
            return NULL;
        }

        Expr* expr = calloc(1, sizeof(Expr));
        expr->type = EXPR_LOGICAL_OP;
        expr->logical.op = LOGICAL_OR;
        expr->logical.left = left_expr;
        expr->logical.right = right_expr;
        return expr;
    }

    /* Single binary comparison */
    char left[MAX_STR_LEN] = {0};
    char op[8] = {0};
    char right[MAX_STR_LEN] = {0};

    const char* ops[] = {">=", "<=", "!=", "<>", "==", "=", ">", "<", NULL};
    char* found_op = NULL;
    int op_idx = -1;

    for (int i = 0; ops[i] != NULL; i++) {
        char* pos = strstr(where_str, ops[i]);
        if (pos) {
            found_op = pos;
            op_idx = i;
            break;
        }
    }

    if (!found_op) return NULL;

    strncpy(op, ops[op_idx], sizeof(op) - 1);
    *found_op = '\0';
    clean_token(left, where_str, sizeof(left));
    clean_token(right, found_op + strlen(ops[op_idx]), sizeof(right));

    return parse_comparison(schema, left, op, right);
}

static PrepareResult prepare_insert(char* input_line, Statement* statement, const Schema* schema) {
    statement->type = STATEMENT_INSERT;

    char* open_paren = strchr(input_line, '(');
    char* close_paren = strrchr(input_line, ')');
    if (!open_paren || !close_paren || open_paren > close_paren) {
        return PREPARE_SYNTAX_ERROR;
    }

    *close_paren = '\0';
    char* val_str = open_paren + 1;

    for (uint32_t i = 0; i < schema->num_columns; i++) {
        while (*val_str && (isspace((unsigned char)*val_str) || *val_str == ',')) val_str++;
        if (!*val_str) return PREPARE_SYNTAX_ERROR;

        char token[MAX_STR_LEN] = {0};
        clean_token(token, val_str, sizeof(token));

        if (*val_str == '\'' || *val_str == '"') {
            char q = *val_str++;
            while (*val_str && *val_str != q) val_str++;
            if (*val_str == q) val_str++;
        } else {
            while (*val_str && *val_str != ',' && !isspace((unsigned char)*val_str)) val_str++;
        }

        statement->row_to_insert.values[i].type = schema->columns[i].type;
        if (schema->columns[i].type == TYPE_INT) {
            statement->row_to_insert.values[i].int_val = atoi(token);
        } else {
            strncpy(statement->row_to_insert.values[i].string_val, token, MAX_STR_LEN);
        }
    }
    return PREPARE_SUCCESS;
}

static PrepareResult prepare_statement(char* input_line, Statement* statement, const Schema* schema) {
    memset(statement, 0, sizeof(Statement));

    while (*input_line && isspace((unsigned char)*input_line)) input_line++;
    size_t len = strlen(input_line);
    while (len > 0 && (input_line[len - 1] == ';' || isspace((unsigned char)input_line[len - 1]))) {
        input_line[--len] = '\0';
    }

    if (strncasecmp(input_line, "insert", 6) == 0) {
        return prepare_insert(input_line, statement, schema);
    }

    if (strncasecmp(input_line, "select", 6) == 0) {
        statement->type = STATEMENT_SELECT;
        char* where_kw = strcasestr(input_line, " where ");
        if (where_kw) {
            *where_kw = '\0';
            statement->where_clause = parse_where_clause(schema, where_kw + 7);
            if (!statement->where_clause) {
                return PREPARE_SYNTAX_ERROR;
            }
        }
        return PREPARE_SUCCESS;
    }

    if (strncasecmp(input_line, "update", 6) == 0) {
        statement->type = STATEMENT_UPDATE;
        char* set_pos = strcasestr(input_line, " set ");
        if (!set_pos) return PREPARE_SYNTAX_ERROR;

        char* where_kw = strcasestr(set_pos, " where ");
        if (where_kw) {
            *where_kw = '\0';
            statement->where_clause = parse_where_clause(schema, where_kw + 7);
            if (!statement->where_clause) return PREPARE_SYNTAX_ERROR;
        }

        char* assign_ptr = set_pos + 5;
        char* eq_pos = strchr(assign_ptr, '=');
        if (!eq_pos) {
            free_expr(statement->where_clause);
            return PREPARE_SYNTAX_ERROR;
        }

        *eq_pos = '\0';
        char col_name[MAX_NAME_LEN] = {0};
        char val_token[MAX_STR_LEN] = {0};
        clean_token(col_name, assign_ptr, sizeof(col_name));
        clean_token(val_token, eq_pos + 1, sizeof(val_token));

        int col = schema_find_column(schema, col_name);
        if (col == -1) {
            free_expr(statement->where_clause);
            return PREPARE_SYNTAX_ERROR;
        }

        /* Check: Disallow in-place modification of PK to keep B+ Tree invariants */
        if (schema->columns[col].is_pk) {
            printf("Error: Primary Key column '%s' cannot be updated in-place.\n", col_name);
            free_expr(statement->where_clause);
            return PREPARE_SYNTAX_ERROR;
        }

        statement->num_assignments = 1;
        statement->assignments[0].col_idx = col;
        statement->assignments[0].value.type = schema->columns[col].type;
        if (schema->columns[col].type == TYPE_INT) {
            statement->assignments[0].value.int_val = atoi(val_token);
        } else {
            strncpy(statement->assignments[0].value.string_val, val_token, MAX_STR_LEN);
        }
        return PREPARE_SUCCESS;
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
}

/* ==================================================================== */
/* Statement Execution                                                  */
/* ==================================================================== */

static ExecuteResult execute_insert(Statement* statement, Table* table) {
    uint32_t key_to_insert = get_pk_value(&table->schema, &statement->row_to_insert);
    Cursor* cursor = tree_find(table, table->root_page_num, key_to_insert);

    void* row_buffer = malloc(table->schema.row_size);
    serialize_row(&table->schema, &statement->row_to_insert, row_buffer);

    ExecuteResult result = leaf_node_insert(cursor, key_to_insert, row_buffer);
    free(row_buffer);
    free(cursor);
    return result;
}

static ExecuteResult execute_select(Statement* statement, Table* table) {
    Cursor* cursor = table_start(table);
    DynamicRow row;

    while (!cursor->end_of_table) {
        deserialize_row(&table->schema, cursor_value(cursor), &row);
        if (evaluate_expr(statement->where_clause, &table->schema, &row)) {
            print_row(&table->schema, &row, statement->selected_cols, statement->num_selected_cols);
        }
        cursor_advance(cursor);
    }
    free(cursor);
    return EXECUTE_SUCCESS;
}

static ExecuteResult execute_update(Statement* statement, Table* table) {
    Cursor* cursor = table_start(table);
    DynamicRow row;
    uint32_t rows_updated = 0;

    while (!cursor->end_of_table) {
        void* slot = cursor_value(cursor);
        deserialize_row(&table->schema, slot, &row);

        if (evaluate_expr(statement->where_clause, &table->schema, &row)) {
            for (int i = 0; i < statement->num_assignments; i++) {
                int col = statement->assignments[i].col_idx;
                row.values[col] = statement->assignments[i].value;
            }
            serialize_row(&table->schema, &row, slot);
            rows_updated++;
        }
        cursor_advance(cursor);
    }
    free(cursor);
    printf("Updated %u row(s).\n", rows_updated);
    return EXECUTE_SUCCESS;
}

static ExecuteResult execute_statement(Statement* statement, Table* table) {
    switch (statement->type) {
        case STATEMENT_INSERT:
            return execute_insert(statement, table);
        case STATEMENT_SELECT:
            return execute_select(statement, table);
        case STATEMENT_UPDATE:
            return execute_update(statement, table);
        default:
            return EXECUTE_ERROR;
    }
}

/* ==================================================================== */
/* REPL & Shell Driver                                                  */
/* ==================================================================== */

static void print_constants(const Schema* schema) {
    printf("PAGE_SIZE: %d\n", PAGE_SIZE);
    printf("COMMON_NODE_HEADER_SIZE: %lu\n", COMMON_NODE_HEADER_SIZE);
    printf("LEAF_NODE_HEADER_SIZE: %lu\n", LEAF_NODE_HEADER_SIZE);
    printf("LEAF_NODE_CELL_SIZE: %u\n", leaf_node_cell_size(schema));
    printf("LEAF_NODE_MAX_CELLS: %u\n", leaf_node_max_cells(schema));
    printf("INTERNAL_NODE_HEADER_SIZE: %lu\n", INTERNAL_NODE_HEADER_SIZE);
    printf("INTERNAL_NODE_MAX_KEYS: %d\n", INTERNAL_NODE_MAX_KEYS);
}

static void print_schema(const Schema* schema) {
    printf("Schema (Row Size %u bytes, Columns: %u):\n", schema->row_size, schema->num_columns);
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        printf("  - %s: %s%s (size: %u, offset: %u)\n",
               schema->columns[i].name,
               schema->columns[i].type == TYPE_INT ? "INT" : "VARCHAR",
               schema->columns[i].is_pk ? " PRIMARY KEY" : "",
               schema->columns[i].size,
               schema->columns[i].offset);
    }
}

static MetaCommandResult do_meta_command(char* input_line, Table* table) {
    if (strcmp(input_line, ".exit") == 0 || strcmp(input_line, "\\q") == 0) {
        db_close(table);
        exit(EXIT_SUCCESS);
    } else if (strcmp(input_line, ".constants") == 0) {
        printf("Constants:\n");
        print_constants(&table->schema);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_line, ".schema") == 0) {
        print_schema(&table->schema);
        return META_COMMAND_SUCCESS;
    }
    return META_COMMAND_UNRECOGNIZED_COMMAND;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <db_filename>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char* filename = argv[1];
    Table* table = db_open(filename);

    char* line = NULL;
    size_t line_cap = 0;

    while (1) {
        printf("tezdb > ");
        fflush(stdout);

        ssize_t bytes_read = getline(&line, &line_cap, stdin);
        if (bytes_read <= 0) {
            printf("\n");
            break;
        }

        while (bytes_read > 0 && (line[bytes_read - 1] == '\n' || line[bytes_read - 1] == '\r')) {
            line[--bytes_read] = '\0';
        }

        if (bytes_read == 0) continue;

        if (line[0] == '.' || line[0] == '\\') {
            switch (do_meta_command(line, table)) {
                case META_COMMAND_SUCCESS:
                    continue;
                case META_COMMAND_UNRECOGNIZED_COMMAND:
                    printf("Unrecognized meta command '%s'\n", line);
                    continue;
            }
        }

        Statement statement;
        switch (prepare_statement(line, &statement, &table->schema)) {
            case PREPARE_SUCCESS:
                break;
            case PREPARE_SYNTAX_ERROR:
                printf("Syntax error. Could not parse statement.\n");
                continue;
            case PREPARE_STRING_TOO_LONG:
                printf("String is too long.\n");
                continue;
            case PREPARE_UNRECOGNIZED_STATEMENT:
                printf("Unrecognized keyword at start of '%s'.\n", line);
                continue;
        }

        switch (execute_statement(&statement, table)) {
            case EXECUTE_SUCCESS:
                printf("Executed.\n");
                break;
            case EXECUTE_DUPLICATE_KEY:
                printf("Error: Duplicate key.\n");
                break;
            case EXECUTE_TABLE_FULL:
                printf("Error: Table full.\n");
                break;
            case EXECUTE_ERROR:
                printf("Execution error.\n");
                break;
        }

        /* Free parsed AST expressions */
        free_expr(statement.where_clause);
    }

    free(line);
    db_close(table);
    return 0;
}
