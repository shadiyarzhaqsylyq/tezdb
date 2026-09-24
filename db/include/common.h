#ifndef COMMON_H
#define COMMON_H

#define _POSIX_C_SOURCE 200809L

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

int strcasecmp_custom(const char* s1, const char* s2);

#endif /* COMMON_H */
