#ifndef DB_BTREE_H
#define DB_BTREE_H

#include "common.h"
#include "pager.h"
#include "schema.h"

// ============================================================================
// Node layout constants
// ============================================================================
#define NODE_TYPE_SIZE           sizeof(uint8_t)
#define NODE_TYPE_OFFSET         0
#define IS_ROOT_SIZE             sizeof(uint8_t)
#define IS_ROOT_OFFSET           (NODE_TYPE_SIZE)
#define PARENT_POINTER_SIZE      sizeof(uint32_t)
#define PARENT_POINTER_OFFSET    (IS_ROOT_OFFSET + IS_ROOT_SIZE)
#define COMMON_NODE_HEADER_SIZE  (NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE)

#define INTERNAL_NODE_NUM_KEYS_SIZE      sizeof(uint32_t)
#define INTERNAL_NODE_NUM_KEYS_OFFSET    (COMMON_NODE_HEADER_SIZE)
#define INTERNAL_NODE_RIGHT_CHILD_SIZE   sizeof(uint32_t)
#define INTERNAL_NODE_RIGHT_CHILD_OFFSET (INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE)
#define INTERNAL_NODE_HEADER_SIZE        (COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE + INTERNAL_NODE_RIGHT_CHILD_SIZE)

#define INTERNAL_NODE_KEY_SIZE   sizeof(uint32_t)
#define INTERNAL_NODE_CHILD_SIZE sizeof(uint32_t)
#define INTERNAL_NODE_CELL_SIZE  (INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE)

#define LEAF_NODE_NUM_CELLS_SIZE   sizeof(uint32_t)
#define LEAF_NODE_NUM_CELLS_OFFSET (COMMON_NODE_HEADER_SIZE)
#define LEAF_NODE_NEXT_LEAF_SIZE   sizeof(uint32_t)
#define LEAF_NODE_NEXT_LEAF_OFFSET (LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE)
#define LEAF_NODE_HEADER_SIZE      (COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE + LEAF_NODE_NEXT_LEAF_SIZE)
#define LEAF_NODE_KEY_SIZE         sizeof(uint32_t)

// ============================================================================
// Table / Cursor
// ============================================================================
struct Table {
    Pager* pager;
    uint32_t root_page_num;
    Schema schema;
    Database* db;
};

struct Cursor {
    Table* table;
    uint32_t page_num;
    uint32_t cell_num;
    bool end_of_table;
};

// ============================================================================
// Node accessors (inline in header for performance)
// ============================================================================
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

static inline uint32_t leaf_node_min_cells(uint32_t row_size) {
    return leaf_node_max_cells(row_size) / 2;
}

static inline uint32_t internal_node_max_keys(void) {
    return (PAGE_SIZE - INTERNAL_NODE_HEADER_SIZE) / INTERNAL_NODE_CELL_SIZE;
}

// ============================================================================
// Public B+tree API
// ============================================================================
void initialize_leaf_node(void* node);
void initialize_internal_node(void* node);

uint32_t get_node_max_key(Table* table, void* node);

Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key);
Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key);
Cursor* table_find(Table* table, uint32_t key);
Cursor* table_start(Table* table);

void* cursor_value(Cursor* cursor);
void  cursor_advance(Cursor* cursor);

void leaf_node_insert(Cursor* cursor, uint32_t key, DynamicRow* value);
void leaf_node_delete(Cursor* cursor);

void print_tree(Table* table, uint32_t page_num, uint32_t indentation_level);

// Needed by database module for page allocation during splits
uint32_t get_unused_page_num(Table* table);


void cursor_normalize(Cursor* cursor);



static inline uint32_t internal_node_min_keys(void) {
    return internal_node_max_keys() / 2;
}

void handle_internal_underflow(Table* table, uint32_t node_page_num);

#endif /* DB_BTREE_H */
