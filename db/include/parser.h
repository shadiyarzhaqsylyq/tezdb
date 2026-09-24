#ifndef PARSER_H
#define PARSER_H

#include "common.h"
#include "schema.h"
#include "row.h"
#include "expr.h"

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

    /* SET-style UPDATE */
    bool is_set_update;
    UpdateAssignment update_assignments[MAX_ASSIGNMENTS];
    uint32_t num_update_assignments;
} Statement;

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

void tokenize_input(const char* input, TokenList* list);
PrepareResult prepare_statement(TokenList* list, const Schema* schema, Statement* statement);

#endif /* PARSER_H */
