#pragma once

#include "Types.hpp"
#include "Pager.hpp"
#include <cstring>
#include <iostream>

enum class NodeType : uint8_t {
    Internal = 0,
    Leaf = 1
};

// Layout constants
inline constexpr size_t NODE_TYPE_OFFSET = 0;
inline constexpr size_t IS_ROOT_OFFSET = sizeof(uint8_t);
inline constexpr size_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + sizeof(uint8_t);
inline constexpr size_t COMMON_NODE_HEADER_SIZE = PARENT_POINTER_OFFSET + sizeof(uint32_t);

// Internal Node Layout
inline constexpr size_t INTERNAL_NODE_NUM_KEYS_OFFSET = COMMON_NODE_HEADER_SIZE;
inline constexpr size_t INTERNAL_NODE_RIGHT_CHILD_OFFSET = INTERNAL_NODE_NUM_KEYS_OFFSET + sizeof(uint32_t);
inline constexpr size_t INTERNAL_NODE_HEADER_SIZE = INTERNAL_NODE_RIGHT_CHILD_OFFSET + sizeof(uint32_t);

inline constexpr size_t INTERNAL_NODE_KEY_SIZE = sizeof(uint32_t);
inline constexpr size_t INTERNAL_NODE_CHILD_SIZE = sizeof(uint32_t);
inline constexpr size_t INTERNAL_NODE_CELL_SIZE = INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE;
inline constexpr uint32_t INTERNAL_NODE_MAX_KEYS = 3;

// Leaf Node Layout
inline constexpr size_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
inline constexpr size_t LEAF_NODE_NEXT_LEAF_OFFSET = LEAF_NODE_NUM_CELLS_OFFSET + sizeof(uint32_t);
inline constexpr size_t LEAF_NODE_HEADER_SIZE = LEAF_NODE_NEXT_LEAF_OFFSET + sizeof(uint32_t);
inline constexpr size_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);

namespace BTree {

inline NodeType get_node_type(const void* node) {
    return static_cast<NodeType>(*reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(node) + NODE_TYPE_OFFSET));
}

inline void set_node_type(void* node, NodeType type) {
    *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(node) + NODE_TYPE_OFFSET) = static_cast<uint8_t>(type);
}

inline bool is_node_root(const void* node) {
    return *reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(node) + IS_ROOT_OFFSET) != 0;
}

inline void set_node_root(void* node, bool is_root) {
    *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(node) + IS_ROOT_OFFSET) = is_root ? 1 : 0;
}

inline uint32_t* node_parent(void* node) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(node) + PARENT_POINTER_OFFSET);
}

inline uint32_t* internal_node_num_keys(void* node) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(node) + INTERNAL_NODE_NUM_KEYS_OFFSET);
}

inline uint32_t* internal_node_right_child(void* node) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(node) + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

inline uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(node) + INTERNAL_NODE_HEADER_SIZE + cell_num * INTERNAL_NODE_CELL_SIZE);
}

inline uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) {
        throw std::runtime_error("Child number out of bounds in internal node.");
    }
    if (child_num == num_keys) {
        return internal_node_right_child(node);
    }
    return internal_node_cell(node, child_num);
}

inline uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(internal_node_cell(node, key_num)) + INTERNAL_NODE_CHILD_SIZE);
}

inline uint32_t leaf_node_cell_size(uint32_t row_size) {
    return LEAF_NODE_KEY_SIZE + row_size;
}

inline uint32_t leaf_node_max_cells(uint32_t row_size) {
    if (row_size == 0) return 0;
    return (PAGE_SIZE - LEAF_NODE_HEADER_SIZE) / leaf_node_cell_size(row_size);
}

inline uint32_t* leaf_node_num_cells(void* node) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(node) + LEAF_NODE_NUM_CELLS_OFFSET);
}

inline uint32_t* leaf_node_next_leaf(void* node) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(node) + LEAF_NODE_NEXT_LEAF_OFFSET);
}

inline void* leaf_node_cell(void* node, uint32_t cell_num, uint32_t row_size) {
    return static_cast<uint8_t*>(node) + LEAF_NODE_HEADER_SIZE + cell_num * leaf_node_cell_size(row_size);
}

inline uint32_t* leaf_node_key(void* node, uint32_t cell_num, uint32_t row_size) {
    return reinterpret_cast<uint32_t*>(leaf_node_cell(node, cell_num, row_size));
}

inline void* leaf_node_value(void* node, uint32_t cell_num, uint32_t row_size) {
    return static_cast<uint8_t*>(leaf_node_cell(node, cell_num, row_size)) + LEAF_NODE_KEY_SIZE;
}

inline void initialize_leaf_node(void* node) {
    set_node_type(node, NodeType::Leaf);
    set_node_root(node, false);
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = 0;
}

inline void initialize_internal_node(void* node) {
    set_node_type(node, NodeType::Internal);
    set_node_root(node, false);
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

inline void serialize_row(const DynamicRow& source, void* destination, const Schema& schema) {
    std::memset(destination, 0, schema.row_size);
    for (size_t i = 0; i < schema.columns.size(); ++i) {
        const auto& col = schema.columns[i];
        auto* dest_ptr = static_cast<uint8_t*>(destination) + col.offset;
        if (i < source.values.size()) {
            const auto& val = source.values[i];
            if (col.type == DataType::Int && std::holds_alternative<int32_t>(val)) {
                int32_t v = std::get<int32_t>(val);
                std::memcpy(dest_ptr, &v, sizeof(int32_t));
            } else if (col.type == DataType::VarChar && std::holds_alternative<std::string>(val)) {
                const auto& str = std::get<std::string>(val);
                size_t copy_len = std::min<size_t>(str.size(), col.length);
                std::memcpy(dest_ptr, str.data(), copy_len);
                dest_ptr[copy_len] = '\0';
            }
        }
    }
}

inline DynamicRow deserialize_row(const void* source, const Schema& schema) {
    DynamicRow destination;
    destination.values.reserve(schema.columns.size());

    for (const auto& col : schema.columns) {
        const auto* src_ptr = static_cast<const uint8_t*>(source) + col.offset;
        if (col.type == DataType::Int) {
            int32_t v = 0;
            std::memcpy(&v, src_ptr, sizeof(int32_t));
            destination.values.emplace_back(v);
        } else {
            std::string s(reinterpret_cast<const char*>(src_ptr));
            destination.values.emplace_back(std::move(s));
        }
    }
    return destination;
}

} // namespace BTree
