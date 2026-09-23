#include "btree.h"
#include "database.h"   /* for allocate_page / free_page / persist_free_list */

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

void update_parent_max_key(Table* table, uint32_t page_num, uint32_t old_max, uint32_t new_max) {
    (void)old_max;
    void* node = pager_get_page(table->pager, page_num);
    if (is_node_root(node)) return;

    uint32_t parent_page_num = *node_parent(node);
    void* parent = pager_get_page(table->pager, parent_page_num);
    uint32_t num_keys = *internal_node_num_keys(parent);

    for (uint32_t i = 0; i < num_keys; i++) {
        if (*internal_node_child(parent, i) == page_num) {
            *internal_node_key(parent, i) = new_max;
            return;
        }
    }

    if (*internal_node_right_child(parent) == page_num) {
        update_parent_max_key(table, parent_page_num, old_max, new_max);
    }
}

void check_and_collapse_root(Table* table) {
    void* root = pager_get_page(table->pager, table->root_page_num);
    if (get_node_type(root) != NODE_INTERNAL) return;

    if (*internal_node_num_keys(root) == 0) {
        uint32_t only_child_page = *internal_node_right_child(root);
        void* only_child = pager_get_page(table->pager, only_child_page);

        memcpy(root, only_child, PAGE_SIZE);
        set_node_root(root, true);

        if (get_node_type(root) == NODE_INTERNAL) {
            uint32_t num_keys = *internal_node_num_keys(root);
            for (uint32_t i = 0; i < num_keys; i++) {
                void* gc = pager_get_page(table->pager, *internal_node_child(root, i));
                *node_parent(gc) = table->root_page_num;
            }
            void* rgc = pager_get_page(table->pager, *internal_node_right_child(root));
            *node_parent(rgc) = table->root_page_num;
        }

        free_page(table->db, only_child_page);
        persist_free_list(table->db);
    }
}

void internal_node_remove_child(Table* table, uint32_t parent_page_num, uint32_t child_page_num) {
    void* parent = pager_get_page(table->pager, parent_page_num);
    uint32_t num_keys = *internal_node_num_keys(parent);

    if (*internal_node_right_child(parent) == child_page_num) {
        if (num_keys == 0) {
            *internal_node_right_child(parent) = INVALID_PAGE_NUM;
            return;
        }
        *internal_node_right_child(parent) = *internal_node_child(parent, num_keys - 1);
        (*internal_node_num_keys(parent))--;
        return;
    }

    int idx = -1;
    for (uint32_t i = 0; i < num_keys; i++) {
        if (*internal_node_child(parent, i) == child_page_num) {
            idx = (int)i;
            break;
        }
    }
    if (idx == -1) return;

    for (uint32_t i = (uint32_t)idx; i < num_keys - 1; i++) {
        void* dest = internal_node_cell(parent, i);
        void* src = internal_node_cell(parent, i + 1);
        memcpy(dest, src, INTERNAL_NODE_CELL_SIZE);
    }
    (*internal_node_num_keys(parent))--;
}

void handle_leaf_underflow(Table* table, uint32_t leaf_page_num) {
    void* leaf = pager_get_page(table->pager, leaf_page_num);
    if (is_node_root(leaf)) return;

    uint32_t row_size = table->schema.row_size;
    uint32_t min_cells = leaf_node_min_cells(row_size);
    if (*leaf_node_num_cells(leaf) >= min_cells) return;

    uint32_t parent_page = *node_parent(leaf);
    void* parent = pager_get_page(table->pager, parent_page);
    uint32_t num_keys = *internal_node_num_keys(parent);

    int my_index = -1;
    bool is_right_child = (*internal_node_right_child(parent) == leaf_page_num);
    if (!is_right_child) {
        for (uint32_t i = 0; i < num_keys; i++) {
            if (*internal_node_child(parent, i) == leaf_page_num) {
                my_index = (int)i;
                break;
            }
        }
        if (my_index == -1) return;
    }

    uint32_t cell_sz = leaf_node_cell_size(row_size);
    uint32_t max_cells = leaf_node_max_cells(row_size);

    uint32_t left_sibling_page = INVALID_PAGE_NUM;
    if (is_right_child) {
        if (num_keys > 0) left_sibling_page = *internal_node_child(parent, num_keys - 1);
    } else if (my_index > 0) {
        left_sibling_page = *internal_node_child(parent, (uint32_t)my_index - 1);
    }

    uint32_t right_sibling_page = INVALID_PAGE_NUM;
    if (!is_right_child) {
        if ((uint32_t)my_index + 1 < num_keys) {
            right_sibling_page = *internal_node_child(parent, (uint32_t)my_index + 1);
        } else {
            right_sibling_page = *internal_node_right_child(parent);
        }
    }

    // Borrow from left
    if (left_sibling_page != INVALID_PAGE_NUM) {
        void* left_sib = pager_get_page(table->pager, left_sibling_page);
        uint32_t left_cells = *leaf_node_num_cells(left_sib);
        if (left_cells > min_cells) {
            uint32_t leaf_cells = *leaf_node_num_cells(leaf);
            for (uint32_t i = leaf_cells; i > 0; i--) {
                memcpy(leaf_node_cell(leaf, i, row_size),
                       leaf_node_cell(leaf, i - 1, row_size), cell_sz);
            }
            memcpy(leaf_node_cell(leaf, 0, row_size),
                   leaf_node_cell(left_sib, left_cells - 1, row_size), cell_sz);
            (*leaf_node_num_cells(leaf))++;
            (*leaf_node_num_cells(left_sib))--;
            uint32_t new_left_max = *leaf_node_key(left_sib, *leaf_node_num_cells(left_sib) - 1, row_size);
            update_parent_max_key(table, left_sibling_page, 0, new_left_max);
            return;
        }
    }

    // Borrow from right
    if (right_sibling_page != INVALID_PAGE_NUM) {
        void* right_sib = pager_get_page(table->pager, right_sibling_page);
        uint32_t right_cells = *leaf_node_num_cells(right_sib);
        if (right_cells > min_cells) {
            uint32_t leaf_cells = *leaf_node_num_cells(leaf);
            memcpy(leaf_node_cell(leaf, leaf_cells, row_size),
                   leaf_node_cell(right_sib, 0, row_size), cell_sz);
            for (uint32_t i = 0; i < right_cells - 1; i++) {
                memcpy(leaf_node_cell(right_sib, i, row_size),
                       leaf_node_cell(right_sib, i + 1, row_size), cell_sz);
            }
            (*leaf_node_num_cells(leaf))++;
            (*leaf_node_num_cells(right_sib))--;
            uint32_t new_leaf_max = *leaf_node_key(leaf, *leaf_node_num_cells(leaf) - 1, row_size);
            update_parent_max_key(table, leaf_page_num, 0, new_leaf_max);
            return;
        }
    }

    // Merge into left
    if (left_sibling_page != INVALID_PAGE_NUM) {
        void* left_sib = pager_get_page(table->pager, left_sibling_page);
        uint32_t left_cells = *leaf_node_num_cells(left_sib);
        uint32_t leaf_cells = *leaf_node_num_cells(leaf);
        if (left_cells + leaf_cells <= max_cells) {
            for (uint32_t i = 0; i < leaf_cells; i++) {
                memcpy(leaf_node_cell(left_sib, left_cells + i, row_size),
                       leaf_node_cell(leaf, i, row_size), cell_sz);
            }
            *leaf_node_num_cells(left_sib) = left_cells + leaf_cells;
            *leaf_node_next_leaf(left_sib) = *leaf_node_next_leaf(leaf);
            internal_node_remove_child(table, parent_page, leaf_page_num);
            free_page(table->db, leaf_page_num);
            persist_free_list(table->db);
            if (*leaf_node_num_cells(left_sib) > 0) {
                uint32_t new_max = *leaf_node_key(left_sib, *leaf_node_num_cells(left_sib) - 1, row_size);
                update_parent_max_key(table, left_sibling_page, 0, new_max);
            }
            check_and_collapse_root(table);
            return;
        }
    }

    // Merge right into leaf
    if (right_sibling_page != INVALID_PAGE_NUM) {
        void* right_sib = pager_get_page(table->pager, right_sibling_page);
        uint32_t leaf_cells = *leaf_node_num_cells(leaf);
        uint32_t right_cells = *leaf_node_num_cells(right_sib);
        if (leaf_cells + right_cells <= max_cells) {
            for (uint32_t i = 0; i < right_cells; i++) {
                memcpy(leaf_node_cell(leaf, leaf_cells + i, row_size),
                       leaf_node_cell(right_sib, i, row_size), cell_sz);
            }
            *leaf_node_num_cells(leaf) = leaf_cells + right_cells;
            *leaf_node_next_leaf(leaf) = *leaf_node_next_leaf(right_sib);
            internal_node_remove_child(table, parent_page, right_sibling_page);
            free_page(table->db, right_sibling_page);
            persist_free_list(table->db);
            if (*leaf_node_num_cells(leaf) > 0) {
                uint32_t new_max = *leaf_node_key(leaf, *leaf_node_num_cells(leaf) - 1, row_size);
                update_parent_max_key(table, leaf_page_num, 0, new_max);
            }
            check_and_collapse_root(table);
            return;
        }
    }
}

void leaf_node_delete(Cursor* cursor) {
    Table* table = cursor->table;
    void* node = pager_get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    uint32_t row_size = table->schema.row_size;

    if (num_cells == 0 || cursor->cell_num >= num_cells) return;

    uint32_t old_max = *leaf_node_key(node, num_cells - 1, row_size);
    bool deleting_max = (cursor->cell_num == num_cells - 1);

    uint32_t cell_sz = leaf_node_cell_size(row_size);
    for (uint32_t i = cursor->cell_num; i < num_cells - 1; i++) {
        memcpy(leaf_node_cell(node, i, row_size),
               leaf_node_cell(node, i + 1, row_size), cell_sz);
    }
    *(leaf_node_num_cells(node)) -= 1;
    if (deleting_max && *leaf_node_num_cells(node) > 0) {
        uint32_t new_max = *leaf_node_key(node, *leaf_node_num_cells(node) - 1, row_size);
        update_parent_max_key(table, cursor->page_num, old_max, new_max);
    }
    handle_leaf_underflow(table, cursor->page_num);
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
    if (original_num_keys >= internal_node_max_keys()) {
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

    uint32_t max_keys = internal_node_max_keys();
    uint32_t split_index = max_keys / 2;

    for (int i = (int)max_keys - 1; i > (int)split_index; i--) {
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
