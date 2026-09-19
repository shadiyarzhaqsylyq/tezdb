#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <algorithm>
#include <stdexcept>

constexpr uint32_t PAGE_SIZE = 4096;
constexpr uint32_t TABLE_MAX_PAGES = 400;
constexpr uint32_t INVALID_PAGE_NUM = UINT32_MAX;
constexpr uint32_t INSERT_PAGE_SAFETY_MARGIN = 8;
constexpr uint32_t SCHEMA_MAGIC = 0x5343484D;

constexpr size_t MAX_COLUMNS = 32;
constexpr size_t MAX_NAME_LEN = 64;

enum class DataType : uint32_t {
    Int = 0,
    VarChar = 1
};

#pragma pack(push, 1)
struct ColumnDefDisk {
    char name[MAX_NAME_LEN];
    DataType type;
    uint32_t length; // Capacity for VARCHAR
    uint32_t offset; // Offset inside row binary
    uint32_t size;   // Byte size stored
    bool is_primary_key;
};

struct SchemaDisk {
    bool has_schema;
    char table_name[MAX_NAME_LEN];
    ColumnDefDisk columns[MAX_COLUMNS];
    uint32_t num_columns;
    uint32_t row_size;
    uint32_t primary_key_index;
};

struct MetaPageDisk {
    uint32_t magic;
    uint32_t root_page_num;
    SchemaDisk schema;
};
#pragma pack(pop)

using Value = std::variant<int32_t, std::string>;

struct ColumnDef {
    std::string name;
    DataType type{DataType::Int};
    uint32_t length{0};
    uint32_t offset{0};
    uint32_t size{0};
    bool is_primary_key{false};

    ColumnDefDisk to_disk() const {
        ColumnDefDisk disk{};
        std::copy_n(name.data(), std::min(name.size(), MAX_NAME_LEN - 1), disk.name);
        disk.type = type;
        disk.length = length;
        disk.offset = offset;
        disk.size = size;
        disk.is_primary_key = is_primary_key;
        return disk;
    }

    static ColumnDef from_disk(const ColumnDefDisk& disk) {
        ColumnDef col;
        col.name = disk.name;
        col.type = disk.type;
        col.length = disk.length;
        col.offset = disk.offset;
        col.size = disk.size;
        col.is_primary_key = disk.is_primary_key;
        return col;
    }
};

class Schema {
public:
    bool has_schema{false};
    std::string table_name;
    std::vector<ColumnDef> columns;
    uint32_t row_size{0};
    uint32_t primary_key_index{0};

    void add_column(const std::string& name, DataType type, uint32_t length, bool is_pk) {
        if (columns.size() >= MAX_COLUMNS) return;

        ColumnDef col;
        col.name = name;
        col.type = type;
        col.is_primary_key = is_pk;
        col.offset = row_size;

        if (type == DataType::Int) {
            col.length = 0;
            col.size = sizeof(int32_t);
        } else {
            col.length = (length > 0) ? length : 32;
            col.size = col.length + 1;
        }

        if (is_pk) {
            primary_key_index = static_cast<uint32_t>(columns.size());
        }

        row_size += col.size;
        columns.push_back(std::move(col));
        has_schema = true;
    }

    [[nodiscard]] int find_column(const std::string& name) const {
        for (size_t i = 0; i < columns.size(); ++i) {
            if (case_insensitive_equal(columns[i].name, name)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    SchemaDisk to_disk() const {
        SchemaDisk disk{};
        disk.has_schema = has_schema;
        std::copy_n(table_name.data(), std::min(table_name.size(), MAX_NAME_LEN - 1), disk.table_name);
        disk.num_columns = static_cast<uint32_t>(columns.size());
        disk.row_size = row_size;
        disk.primary_key_index = primary_key_index;
        for (size_t i = 0; i < columns.size(); ++i) {
            disk.columns[i] = columns[i].to_disk();
        }
        return disk;
    }

    static Schema from_disk(const SchemaDisk& disk) {
        Schema schema;
        schema.has_schema = disk.has_schema;
        schema.table_name = disk.table_name;
        schema.row_size = disk.row_size;
        schema.primary_key_index = disk.primary_key_index;
        for (uint32_t i = 0; i < disk.num_columns; ++i) {
            schema.columns.push_back(ColumnDef::from_disk(disk.columns[i]));
        }
        return schema;
    }

private:
    static bool case_insensitive_equal(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        return std::equal(a.begin(), a.end(), b.begin(), [](char c1, char c2) {
            return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2));
        });
    }
};

struct DynamicRow {
    std::vector<Value> values;

    [[nodiscard]] uint32_t get_pk_value(const Schema& schema) const {
        if (schema.primary_key_index < values.size()) {
            if (std::holds_alternative<int32_t>(values[schema.primary_key_index])) {
                return static_cast<uint32_t>(std::get<int32_t>(values[schema.primary_key_index]));
            }
        }
        return 0;
    }
};
