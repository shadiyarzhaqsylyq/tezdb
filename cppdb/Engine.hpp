#pragma once

#include "Types.hpp"
#include "Pager.hpp"
#include "BTree.hpp"
#include "Parser.hpp"
#include <vector>
#include <memory>

enum class ExecuteResult {
    Success,
    DuplicateKey,
    NotFound,
    TxAlreadyActive,
    NoActiveTx,
    TableFull
};

class Table;

class Cursor {
public:
    Cursor(Table* table, uint32_t page_num, uint32_t cell_num, bool end_of_table = false)
        : table_(table), page_num_(page_num), cell_num_(cell_num), end_of_table_(end_of_table) {}

    [[nodiscard]] void* value();
    void advance();
    void delete_current_leaf();

    [[nodiscard]] bool is_end() const noexcept { return end_of_table_; }
    [[nodiscard]] uint32_t page_num() const noexcept { return page_num_; }
    [[nodiscard]] uint32_t cell_num() const noexcept { return cell_num_; }

private:
    Table* table_;
    uint32_t page_num_;
    uint32_t cell_num_;
    bool end_of_table_;
};

class Table {
public:
    explicit Table(const std::string& filename);
    ~Table();

    [[nodiscard]] std::unique_ptr<Cursor> find(uint32_t key);
    [[nodiscard]] std::unique_ptr<Cursor> start();

    ExecuteResult execute(Statement& stmt);
    void print_tree(uint32_t page_num, uint32_t indentation_level);
    void print_constants() const;

    [[nodiscard]] const Schema& schema() const noexcept { return schema_; }
    [[nodiscard]] uint32_t root_page_num() const noexcept { return root_page_num_; }

    friend class Cursor;

private:
    uint32_t get_unused_page_num() noexcept { return pager_.num_pages(); }
    uint32_t get_node_max_key(void* node);
    std::unique_ptr<Cursor> leaf_node_find(uint32_t page_num, uint32_t key);
    std::unique_ptr<Cursor> internal_node_find(uint32_t page_num, uint32_t key);
    uint32_t internal_node_find_child(void* node, uint32_t key);

    void leaf_node_insert(Cursor& cursor, uint32_t key, const DynamicRow& value);
    void leaf_node_split_and_insert(Cursor& cursor, uint32_t key, const DynamicRow& value);
    void internal_node_insert(uint32_t parent_page_num, uint32_t child_page_num);
    void internal_node_split_and_insert(uint32_t parent_page_num, uint32_t child_page_num);
    void create_new_root(uint32_t right_child_page_num);
    void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key);

    ExecuteResult execute_insert(const Statement& stmt);
    ExecuteResult execute_select(const Statement& stmt);
    ExecuteResult execute_update(const Statement& stmt);
    ExecuteResult execute_delete(const Statement& stmt);
    ExecuteResult execute_begin();
    ExecuteResult execute_commit();
    ExecuteResult execute_rollback();

    Pager pager_;
    uint32_t root_page_num_{1};
    Schema schema_;

    // Transactions
    bool in_transaction_{false};
    uint32_t tx_original_num_pages_{0};
    std::array<std::unique_ptr<uint8_t[]>, TABLE_MAX_PAGES> tx_backup_{};
    std::array<bool, TABLE_MAX_PAGES> tx_was_cached_{};
};
