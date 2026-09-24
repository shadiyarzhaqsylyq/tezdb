#ifndef DB_PARSER_H
#define DB_PARSER_H

#include "common.h"
#include "schema.h"
#include "database.h"

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
PrepareResult prepare_statement(TokenList* list, const Database* db, Statement* statement);

#endif /* DB_PARSER_H */
