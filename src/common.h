#ifndef DB_COMMON_H
#define DB_COMMON_H

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

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
// Constants
// ============================================================================
#define PAGE_SIZE 4096
#define TABLE_MAX_PAGES 400
#define INVALID_PAGE_NUM UINT32_MAX
#define INSERT_PAGE_SAFETY_MARGIN 8

#define SCHEMA_MAGIC_V1_SINGLE_TABLE 0x5343484D
#define SCHEMA_MAGIC_V2_NO_FREELIST  0x53434832
#define SCHEMA_MAGIC                 0x53434833

#define MAX_COLUMNS     32
#define MAX_NAME_LEN    64
#define MAX_STR_LEN     256
#define MAX_TOKENS      256
#define MAX_ASSIGNMENTS 32
#define MAX_TABLES      16

#define CATALOG_START_PAGE 1
#define FREE_LIST_PAGE     (CATALOG_START_PAGE + MAX_TABLES)

// ============================================================================
// Data Types
// ============================================================================
typedef enum DataType {
    DATA_TYPE_INT = 0,
    DATA_TYPE_VARCHAR = 1
} DataType;

typedef struct ColumnDef {
    char name[MAX_NAME_LEN];
    DataType type;
    uint32_t length;
    uint32_t offset;
    uint32_t size;
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

typedef struct Value {
    DataType type;
    int32_t int_val;
    char str_val[MAX_STR_LEN];
} Value;

typedef struct DynamicRow {
    Value values[MAX_COLUMNS];
    uint32_t num_values;
} DynamicRow;

// ============================================================================
// Result / Statement enums
// ============================================================================
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
// Expression / WHERE
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
    char column[MAX_NAME_LEN];
    WhereOp op;
    Literal value;
    LogicalOp log_op;
    struct Expr* left;
    struct Expr* right;
} Expr;

typedef struct UpdateAssignment {
    char column_name[MAX_NAME_LEN];
    char value_text[MAX_STR_LEN];
} UpdateAssignment;

// ============================================================================
// Statement (used by parser + executor)
// ============================================================================
typedef struct Statement {
    StatementType type;
    DynamicRow row_to_insert;
    char table_name[MAX_NAME_LEN];
    Schema created_schema;

    Expr* where;
    uint32_t target_id;

    bool has_join;
    char join_table_name[MAX_NAME_LEN];
    char join_left_column[MAX_NAME_LEN];
    char join_right_column[MAX_NAME_LEN];
    WhereOp join_op;

    bool select_all;
    char select_columns[MAX_COLUMNS][MAX_NAME_LEN];
    uint32_t select_column_indices[MAX_COLUMNS];
    uint32_t num_select_columns;

    AggregateType agg_type;
    char agg_column[MAX_NAME_LEN];
    bool has_order_by;
    char order_by_column[MAX_NAME_LEN];
    bool order_by_desc;
    bool has_limit;
    uint32_t limit_count;
    bool has_offset;
    uint32_t offset_count;

    bool is_set_update;
    UpdateAssignment update_assignments[MAX_ASSIGNMENTS];
    uint32_t num_update_assignments;

    AlterKind alter_kind;
    char alter_column_name[MAX_NAME_LEN];
    char alter_new_name[MAX_NAME_LEN];
    DataType alter_column_type;
    uint32_t alter_column_length;
} Statement;

// Forward declarations for types defined in other headers
typedef struct Pager Pager;
typedef struct Database Database;
typedef struct Table Table;
typedef struct Cursor Cursor;
typedef struct TableCatalogEntry TableCatalogEntry;

#endif /* DB_COMMON_H */
