#include "Engine.hpp"
#include <iostream>

void* Cursor::value() {
    void* page = table_->pager_.get_page(page_num_);
    return BTree::leaf_node_value(page, cell_num_, table_->schema_.row_size);
}

void Cursor::advance() {
    void* node = table_->pager_.get_page(page_num_);
    cell_num_++;
    if (cell_num_ >= *BTree::leaf_node_num_cells(node)) {
        uint32_t next_page = *BTree::leaf_node_next_leaf(node);
        if (next_page == 0) {
            end_of_table_ = true;
        } else {
            page_num_ = next_page;
            cell_num_ = 0;
        }
    }
}

void Cursor::delete_current_leaf() {
    void* node = table_->pager_.get_page(page_num_);
    uint32_t num_cells = *BTree::leaf_node_num_cells(node);
    uint32_t cell_sz = BTree::leaf_node_cell_size(table_->schema_.row_size);

    for (uint32_t i = cell_num_; i < num_cells - 1; ++i) {
        std::memcpy(BTree::leaf_node_cell(node, i, table_->schema_.row_size),
                    BTree::leaf_node_cell(node, i + 1, table_->schema_.row_size), cell_sz);
    }
    *BTree::leaf_node_num_cells(node) -= 1;
}

Table::Table(const std::string& filename) : pager_(filename) {
    if (pager_.num_pages() == 0) {
        root_page_num_ = 1;
        schema_ = Schema{};

        auto* meta = reinterpret_cast<MetaPageDisk*>(pager_.get_page(0));
        meta->magic = SCHEMA_MAGIC;
        meta->root_page_num = root_page_num_;
        meta->schema = schema_.to_disk();

        void* root_node = pager_.get_page(root_page_num_);
        BTree::initialize_leaf_node(root_node);
        BTree::set_node_root(root_node, true);
    } else {
        auto* meta = reinterpret_cast<MetaPageDisk*>(pager_.get_page(0));
        if (meta->magic == SCHEMA_MAGIC) {
            root_page_num_ = meta->root_page_num;
            schema_ = Schema::from_disk(meta->schema);
        } else {
            root_page_num_ = 1;
            schema_ = Schema{};
        }
    }
}

Table::~Table() {
    try {
        pager_.flush_all();
    } catch (...) {
        // Suppress destructors throw
    }
}

uint32_t Table::get_node_max_key(void* node) {
    if (BTree::get_node_type(node) == NodeType::Leaf) {
        return *BTree::leaf_node_key(node, *BTree::leaf_node_num_cells(node) - 1, schema_.row_size);
    }
    void* right_child = pager_.get_page(*BTree::internal_node_right_child(node));
    return get_node_max_key(right_child);
}

std::unique_ptr<Cursor> Table::leaf_node_find(uint32_t page_num, uint32_t key) {
    void* node = pager_.get_page(page_num);
    uint32_t num_cells = *BTree::leaf_node_num_cells(node);

    uint32_t min_index = 0;
    uint32_t one_past_max_index = num_cells;
    while (one_past_max_index != min_index) {
        uint32_t index = (min_index + one_past_max_index) / 2;
        uint32_t key_at_index = *BTree::leaf_node_key(node, index, schema_.row_size);
        if (key == key_at_index) {
            return std::make_unique<Cursor>(this, page_num, index);
        }
        if (key < key_at_index) {
            one_past_max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return std::make_unique<Cursor>(this, page_num, min_index);
}

uint32_t Table::internal_node_find_child(void* node, uint32_t key) {
    uint32_t num_keys = *BTree::internal_node_num_keys(node);
    uint32_t min_index = 0;
    uint32_t max_index = num_keys;
    while (min_index != max_index) {
        uint32_t index = (min_index + max_index) / 2;
        uint32_t key_to_right = *BTree::internal_node_key(node, index);
        if (key_to_right >= key) {
            max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return min_index;
}

std::unique_ptr<Cursor> Table::internal_node_find(uint32_t page_num, uint32_t key) {
    void* node = pager_.get_page(page_num);
    uint32_t child_index = internal_node_find_child(node, key);
    uint32_t child_num = *BTree::internal_node_child(node, child_index);
    void* child = pager_.get_page(child_num);

    if (BTree::get_node_type(child) == NodeType::Leaf) {
        return leaf_node_find(child_num, key);
    }
    return internal_node_find(child_num, key);
}

std::unique_ptr<Cursor> Table::find(uint32_t key) {
    void* root_node = pager_.get_page(root_page_num_);
    if (BTree::get_node_type(root_node) == NodeType::Leaf) {
        return leaf_node_find(root_page_num_, key);
    }
    return internal_node_find(root_page_num_, key);
}

std::unique_ptr<Cursor> Table::start() {
    auto cursor = find(0);
    void* node = pager_.get_page(cursor->page_num());
    bool end = (*BTree::leaf_node_num_cells(node) == 0);
    return std::make_unique<Cursor>(this, cursor->page_num(), cursor->cell_num(), end);
}

void Table::create_new_root(uint32_t right_child_page_num) {
    void* root = pager_.get_page(root_page_num_);
    void* right_child = pager_.get_page(right_child_page_num);
    uint32_t left_child_page_num = get_unused_page_num();
    void* left_child = pager_.get_page(left_child_page_num);

    if (BTree::get_node_type(root) == NodeType::Internal) {
        BTree::initialize_internal_node(right_child);
        BTree::initialize_internal_node(left_child);
    }

    std::memcpy(left_child, root, PAGE_SIZE);
    BTree::set_node_root(left_child, false);

    if (BTree::get_node_type(left_child) == NodeType::Internal) {
        for (uint32_t i = 0; i < *BTree::internal_node_num_keys(left_child); ++i) {
            void* child = pager_.get_page(*BTree::internal_node_child(left_child, i));
            *BTree::node_parent(child) = left_child_page_num;
        }
        void* child = pager_.get_page(*BTree::internal_node_right_child(left_child));
        *BTree::node_parent(child) = left_child_page_num;
    }

    BTree::initialize_internal_node(root);
    BTree::set_node_root(root, true);
    *BTree::internal_node_num_keys(root) = 1;
    *BTree::internal_node_child(root, 0) = left_child_page_num;
    *BTree::internal_node_key(root, 0) = get_node_max_key(left_child);
    *BTree::internal_node_right_child(root) = right_child_page_num;
    *BTree::node_parent(left_child) = root_page_num_;
    *BTree::node_parent(right_child) = root_page_num_;
}

void Table::update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
    uint32_t old_child_index = internal_node_find_child(node, old_key);
    *BTree::internal_node_key(node, old_child_index) = new_key;
}

void Table::internal_node_insert(uint32_t parent_page_num, uint32_t child_page_num) {
    void* parent = pager_.get_page(parent_page_num);
    void* child = pager_.get_page(child_page_num);
    uint32_t child_max_key = get_node_max_key(child);
    uint32_t index = internal_node_find_child(parent, child_max_key);

    uint32_t original_num_keys = *BTree::internal_node_num_keys(parent);
    if (original_num_keys >= INTERNAL_NODE_MAX_KEYS) {
        internal_node_split_and_insert(parent_page_num, child_page_num);
        return;
    }

    uint32_t right_child_page_num = *BTree::internal_node_right_child(parent);
    if (right_child_page_num == INVALID_PAGE_NUM) {
        *BTree::internal_node_right_child(parent) = child_page_num;
        return;
    }

    void* right_child = pager_.get_page(right_child_page_num);
    *BTree::internal_node_num_keys(parent) = original_num_keys + 1;

    if (child_max_key > get_node_max_key(right_child)) {
        *BTree::internal_node_child(parent, original_num_keys) = right_child_page_num;
        *BTree::internal_node_key(parent, original_num_keys) = get_node_max_key(right_child);
        *BTree::internal_node_right_child(parent) = child_page_num;
    } else {
        for (uint32_t i = original_num_keys; i > index; --i) {
            void* dest = BTree::internal_node_cell(parent, i);
            void* src = BTree::internal_node_cell(parent, i - 1);
            std::memcpy(dest, src, INTERNAL_NODE_CELL_SIZE);
        }
        *BTree::internal_node_child(parent, index) = child_page_num;
        *BTree::internal_node_key(parent, index) = child_max_key;
    }
}

void Table::internal_node_split_and_insert(uint32_t parent_page_num, uint32_t child_page_num) {
    uint32_t old_page_num = parent_page_num;
    void* old_node = pager_.get_page(parent_page_num);
    uint32_t old_max = get_node_max_key(old_node);

    void* child = pager_.get_page(child_page_num);
    uint32_t child_max = get_node_max_key(child);

    uint32_t new_page_num = get_unused_page_num();
    bool splitting_root = BTree::is_node_root(old_node);

    void* parent = nullptr;
    void* new_node = nullptr;

    if (splitting_root) {
        create_new_root(new_page_num);
        parent = pager_.get_page(root_page_num_);
        old_page_num = *BTree::internal_node_child(parent, 0);
        old_node = pager_.get_page(old_page_num);
    } else {
        parent = pager_.get_page(*BTree::node_parent(old_node));
        new_node = pager_.get_page(new_page_num);
        BTree::initialize_internal_node(new_node);
    }

    uint32_t* old_num_keys = BTree::internal_node_num_keys(old_node);
    uint32_t cur_page_num = *BTree::internal_node_right_child(old_node);
    void* cur = pager_.get_page(cur_page_num);

    internal_node_insert(new_page_num, cur_page_num);
    *BTree::node_parent(cur) = new_page_num;
    *BTree::internal_node_right_child(old_node) = INVALID_PAGE_NUM;

    for (int i = INTERNAL_NODE_MAX_KEYS - 1; i > static_cast<int>(INTERNAL_NODE_MAX_KEYS / 2); --i) {
        cur_page_num = *BTree::internal_node_child(old_node, i);
        cur = pager_.get_page(cur_page_num);
        internal_node_insert(new_page_num, cur_page_num);
        *BTree::node_parent(cur) = new_page_num;
        (*old_num_keys)--;
    }

    *BTree::internal_node_right_child(old_node) = *BTree::internal_node_child(old_node, *old_num_keys - 1);
    (*old_num_keys)--;

    uint32_t max_after_split = get_node_max_key(old_node);
    uint32_t dest_page = (child_max < max_after_split) ? old_page_num : new_page_num;

    internal_node_insert(dest_page, child_page_num);
    *BTree::node_parent(child) = dest_page;

    update_internal_node_key(parent, old_max, get_node_max_key(old_node));

    if (!splitting_root) {
        internal_node_insert(*BTree::node_parent(old_node), new_page_num);
        *BTree::node_parent(new_node) = *BTree::node_parent(old_node);
    }
}

void Table::leaf_node_split_and_insert(Cursor& cursor, uint32_t key, const DynamicRow& value) {
    void* old_node = pager_.get_page(cursor.page_num());
    uint32_t old_max = get_node_max_key(old_node);
    uint32_t new_page_num = get_unused_page_num();
    void* new_node = pager_.get_page(new_page_num);

    BTree::initialize_leaf_node(new_node);
    *BTree::node_parent(new_node) = *BTree::node_parent(old_node);
    *BTree::leaf_node_next_leaf(new_node) = *BTree::leaf_node_next_leaf(old_node);
    *BTree::leaf_node_next_leaf(old_node) = new_page_num;

    uint32_t max_cells = BTree::leaf_node_max_cells(schema_.row_size);
    uint32_t right_split = (max_cells + 1) / 2;
    uint32_t left_split = (max_cells + 1) - right_split;
    uint32_t cell_sz = BTree::leaf_node_cell_size(schema_.row_size);

    for (int32_t i = static_cast<int32_t>(max_cells); i >= 0; --i) {
        void* dest_node = (static_cast<uint32_t>(i) >= left_split) ? new_node : old_node;
        uint32_t index_within_node = static_cast<uint32_t>(i) % left_split;
        void* destination = BTree::leaf_node_cell(dest_node, index_within_node, schema_.row_size);

        if (static_cast<uint32_t>(i) == cursor.cell_num()) {
            BTree::serialize_row(value, BTree::leaf_node_value(dest_node, index_within_node, schema_.row_size), schema_);
            *BTree::leaf_node_key(dest_node, index_within_node, schema_.row_size) = key;
        } else if (static_cast<uint32_t>(i) > cursor.cell_num()) {
            std::memcpy(destination, BTree::leaf_node_cell(old_node, i - 1, schema_.row_size), cell_sz);
        } else {
            std::memcpy(destination, BTree::leaf_node_cell(old_node, i, schema_.row_size), cell_sz);
        }
    }

    *BTree::leaf_node_num_cells(old_node) = left_split;
    *BTree::leaf_node_num_cells(new_node) = right_split;

    if (BTree::is_node_root(old_node)) {
        create_new_root(new_page_num);
    } else {
        uint32_t parent_page_num = *BTree::node_parent(old_node);
        uint32_t new_max = get_node_max_key(old_node);
        void* parent = pager_.get_page(parent_page_num);
        update_internal_node_key(parent, old_max, new_max);
        internal_node_insert(parent_page_num, new_page_num);
    }
}

void Table::leaf_node_insert(Cursor& cursor, uint32_t key, const DynamicRow& value) {
    void* node = pager_.get_page(cursor.page_num());
    uint32_t num_cells = *BTree::leaf_node_num_cells(node);
    uint32_t max_cells = BTree::leaf_node_max_cells(schema_.row_size);

    if (num_cells >= max_cells) {
        leaf_node_split_and_insert(cursor, key, value);
        return;
    }
    uint32_t cell_sz = BTree::leaf_node_cell_size(schema_.row_size);
    if (cursor.cell_num() < num_cells) {
        for (uint32_t i = num_cells; i > cursor.cell_num(); --i) {
            std::memcpy(BTree::leaf_node_cell(node, i, schema_.row_size),
                        BTree::leaf_node_cell(node, i - 1, schema_.row_size), cell_sz);
        }
    }
    *BTree::leaf_node_num_cells(node) += 1;
    *BTree::leaf_node_key(node, cursor.cell_num(), schema_.row_size) = key;
    BTree::serialize_row(value, BTree::leaf_node_value(node, cursor.cell_num(), schema_.row_size), schema_);
}

ExecuteResult Table::execute_insert(const Statement& stmt) {
    if (pager_.num_pages() + INSERT_PAGE_SAFETY_MARGIN > TABLE_MAX_PAGES) {
        return ExecuteResult::TableFull;
    }

    uint32_t key_to_insert = stmt.row_to_insert.get_pk_value(schema_);
    auto cursor = find(key_to_insert);

    void* node = pager_.get_page(cursor->page_num());
    uint32_t num_cells = *BTree::leaf_node_num_cells(node);
    if (cursor->cell_num() < num_cells) {
        uint32_t key_at_index = *BTree::leaf_node_key(node, cursor->cell_num(), schema_.row_size);
        if (key_at_index == key_to_insert) {
            return ExecuteResult::DuplicateKey;
        }
    }

    leaf_node_insert(*cursor, key_to_insert, stmt.row_to_insert);
    return ExecuteResult::Success;
}

ExecuteResult Table::execute_select(const Statement& stmt) {
    auto cursor = start();
    uint32_t match_count = 0;

    while (!cursor->is_end()) {
        DynamicRow row = BTree::deserialize_row(cursor->value(), schema_);
        if (!stmt.where_clause || stmt.where_clause->evaluate(row, schema_)) {
            match_count++;
            if (!stmt.is_count) {
                std::cout << "(";
                for (size_t i = 0; i < schema_.columns.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    if (schema_.columns[i].type == DataType::Int) {
                        std::cout << std::get<int32_t>(row.values[i]);
                    } else {
                        std::cout << "'" << std::get<std::string>(row.values[i]) << "'";
                    }
                }
                std::cout << ")\n";
            }
        }
        cursor->advance();
    }

    if (stmt.is_count) {
        std::cout << match_count << " row(s).\n";
    }
    return ExecuteResult::Success;
}

ExecuteResult Table::execute_update(const Statement& stmt) {
    struct PendingUpdate {
        uint32_t old_key;
        uint32_t new_key;
        DynamicRow row;
    };
    std::vector<PendingUpdate> pending;

    auto cursor = start();
    while (!cursor->is_end()) {
        DynamicRow row = BTree::deserialize_row(cursor->value(), schema_);
        if (!stmt.where_clause || stmt.where_clause->evaluate(row, schema_)) {
            uint32_t old_key = row.get_pk_value(schema_);

            for (const auto& assign : stmt.update_assignments) {
                int col_idx = schema_.find_column(assign.column_name);
                if (col_idx >= 0 && static_cast<size_t>(col_idx) < row.values.size()) {
                    if (schema_.columns[col_idx].type == DataType::Int) {
                        row.values[col_idx] = static_cast<int32_t>(std::stoi(assign.value_text));
                    } else {
                        row.values[col_idx] = assign.value_text;
                    }
                }
            }
            pending.push_back({old_key, row.get_pk_value(schema_), std::move(row)});
        }
        cursor->advance();
    }

    uint32_t applied = 0;
    uint32_t skipped_duplicates = 0;

    for (const auto& u : pending) {
        if (u.new_key == u.old_key) {
            auto c = find(u.old_key);
            void* node = pager_.get_page(c->page_num());
            uint32_t num_cells = *BTree::leaf_node_num_cells(node);
            if (c->cell_num() < num_cells &&
                *BTree::leaf_node_key(node, c->cell_num(), schema_.row_size) == u.old_key) {
                BTree::serialize_row(u.row, c->value(), schema_);
                applied++;
            }
            continue;
        }

        auto dest = find(u.new_key);
        void* dest_node = pager_.get_page(dest->page_num());
        uint32_t dest_num_cells = *BTree::leaf_node_num_cells(dest_node);
        bool duplicate = (dest->cell_num() < dest_num_cells &&
                          *BTree::leaf_node_key(dest_node, dest->cell_num(), schema_.row_size) == u.new_key);

        if (duplicate) {
            skipped_duplicates++;
            continue;
        }

        auto old_cursor = find(u.old_key);
        void* old_node = pager_.get_page(old_cursor->page_num());
        uint32_t old_num_cells = *BTree::leaf_node_num_cells(old_node);
        if (old_cursor->cell_num() < old_num_cells &&
            *BTree::leaf_node_key(old_node, old_cursor->cell_num(), schema_.row_size) == u.old_key) {
            old_cursor->delete_current_leaf();

            auto insert_cursor = find(u.new_key);
            leaf_node_insert(*insert_cursor, u.new_key, u.row);
            applied++;
        }
    }

    if (skipped_duplicates > 0) {
        std::cout << "UPDATE " << applied << " (skipped " << skipped_duplicates << " due to duplicate key)\n";
    } else {
        std::cout << "UPDATE " << applied << "\n";
    }
    return ExecuteResult::Success;
}

ExecuteResult Table::execute_delete(const Statement& stmt) {
    std::vector<uint32_t> keys_to_delete;
    auto cursor = start();

    while (!cursor->is_end()) {
        DynamicRow row = BTree::deserialize_row(cursor->value(), schema_);
        if (!stmt.where_clause || stmt.where_clause->evaluate(row, schema_)) {
            keys_to_delete.push_back(row.get_pk_value(schema_));
        }
        cursor->advance();
    }

    for (uint32_t key : keys_to_delete) {
        auto c = find(key);
        void* node = pager_.get_page(c->page_num());
        uint32_t num_cells = *BTree::leaf_node_num_cells(node);
        if (c->cell_num() < num_cells &&
            *BTree::leaf_node_key(node, c->cell_num(), schema_.row_size) == key) {
            c->delete_current_leaf();
        }
    }
    std::cout << "DELETE " << keys_to_delete.size() << "\n";
    return ExecuteResult::Success;
}

ExecuteResult Table::execute_begin() {
    if (in_transaction_) return ExecuteResult::TxAlreadyActive;

    in_transaction_ = true;
    tx_original_num_pages_ = pager_.num_pages();

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; ++i) {
        if (pager_.has_page(i)) {
            tx_backup_[i] = std::make_unique<uint8_t[]>(PAGE_SIZE);
            std::memcpy(tx_backup_[i].get(), pager_.get_page(i), PAGE_SIZE);
            tx_was_cached_[i] = true;
        } else {
            tx_backup_[i].reset();
            tx_was_cached_[i] = false;
        }
    }
    return ExecuteResult::Success;
}

ExecuteResult Table::execute_commit() {
    if (!in_transaction_) return ExecuteResult::NoActiveTx;

    for (auto& backup : tx_backup_) {
        backup.reset();
    }
    pager_.flush_all();
    in_transaction_ = false;
    return ExecuteResult::Success;
}

ExecuteResult Table::execute_rollback() {
    if (!in_transaction_) return ExecuteResult::NoActiveTx;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; ++i) {
        if (i < tx_original_num_pages_) {
            if (tx_was_cached_[i]) {
                std::memcpy(pager_.get_page(i), tx_backup_[i].get(), PAGE_SIZE);
                tx_backup_[i].reset();
            } else if (pager_.has_page(i)) {
                pager_.drop_page(i);
            }
        } else {
            pager_.drop_page(i);
            tx_backup_[i].reset();
        }
    }
    pager_.set_num_pages(tx_original_num_pages_);
    in_transaction_ = false;
    return ExecuteResult::Success;
}

ExecuteResult Table::execute(Statement& stmt) {
    switch (stmt.type) {
        case StatementType::Insert:
            return execute_insert(stmt);
        case StatementType::Select:
            return execute_select(stmt);
        case StatementType::Update:
            return execute_update(stmt);
        case StatementType::Delete:
            return execute_delete(stmt);
        case StatementType::Create: {
            schema_ = std::move(stmt.created_schema);

            auto* meta = reinterpret_cast<MetaPageDisk*>(pager_.get_page(0));
            meta->magic = SCHEMA_MAGIC;
            meta->root_page_num = root_page_num_;
            meta->schema = schema_.to_disk();

            void* root_node = pager_.get_page(root_page_num_);
            BTree::initialize_leaf_node(root_node);
            BTree::set_node_root(root_node, true);
            std::cout << "CREATE TABLE (" << schema_.columns.size() << " columns configured)\n";
            return ExecuteResult::Success;
        }
        case StatementType::Begin:
            return execute_begin();
        case StatementType::Commit:
            return execute_commit();
        case StatementType::Rollback:
            return execute_rollback();
    }
    return ExecuteResult::Success;
}

void Table::print_tree(uint32_t page_num, uint32_t indentation_level) {
    void* node = pager_.get_page(page_num);
    std::string indent(indentation_level * 2, ' ');

    if (BTree::get_node_type(node) == NodeType::Leaf) {
        uint32_t num_keys = *BTree::leaf_node_num_cells(node);
        std::cout << indent << "- leaf (size " << num_keys << ")\n";
        for (uint32_t i = 0; i < num_keys; ++i) {
            std::cout << indent << "  - " << *BTree::leaf_node_key(node, i, schema_.row_size) << "\n";
        }
    } else {
        uint32_t num_keys = *BTree::internal_node_num_keys(node);
        std::cout << indent << "- internal (size " << num_keys << ")\n";
        if (num_keys > 0) {
            for (uint32_t i = 0; i < num_keys; ++i) {
                uint32_t child = *BTree::internal_node_child(node, i);
                print_tree(child, indentation_level + 1);
                std::cout << indent << "  - key " << *BTree::internal_node_key(node, i) << "\n";
            }
            uint32_t child = *BTree::internal_node_right_child(node);
            print_tree(child, indentation_level + 1);
        }
    }
}

void Table::print_constants() const {
    std::cout << "ROW_SIZE: " << schema_.row_size << "\n"
              << "COMMON_NODE_HEADER_SIZE: " << COMMON_NODE_HEADER_SIZE << "\n"
              << "LEAF_NODE_HEADER_SIZE: " << LEAF_NODE_HEADER_SIZE << "\n"
              << "LEAF_NODE_CELL_SIZE: " << BTree::leaf_node_cell_size(schema_.row_size) << "\n"
              << "LEAF_NODE_MAX_CELLS: " << BTree::leaf_node_max_cells(schema_.row_size) << "\n";
}
