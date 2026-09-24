#include "parser.h"

void tokenize_input(const char* input, TokenList* list) {
    list->count = 0;
    list->cursor = 0;
    size_t pos = 0;
    size_t len = strlen(input);

    while (pos < len && list->count < MAX_TOKENS - 1) {
        if (isspace((unsigned char)input[pos])) {
            pos++;
            continue;
        }

        if (input[pos] == ';') {
            list->tokens[list->count].kind = TOKEN_SYMBOL;
            strcpy(list->tokens[list->count].text, ";");
            list->count++;
            pos++;
            continue;
        }

        if (input[pos] == '\'') {
            char str[MAX_STR_LEN] = {0};
            size_t str_pos = 0;
            pos++;
            while (pos < len && input[pos] != '\'' && str_pos < MAX_STR_LEN - 1) {
                str[str_pos++] = input[pos++];
            }
            str[str_pos] = '\0';
            if (pos < len && input[pos] == '\'') pos++;

            list->tokens[list->count].kind = TOKEN_STRING_LITERAL;
            strcpy(list->tokens[list->count].text, str);
            list->count++;
            continue;
        }

        /* Two-character operators: <=, >=, !=, <> */
        if (pos + 1 < len) {
            char op2[3] = {input[pos], input[pos + 1], '\0'};
            if (strcmp(op2, "<=") == 0 || strcmp(op2, ">=") == 0 ||
                strcmp(op2, "!=") == 0 || strcmp(op2, "<>") == 0) {
                list->tokens[list->count].kind = TOKEN_SYMBOL;
                strcpy(list->tokens[list->count].text, op2);
                list->count++;
                pos += 2;
                continue;
            }
        }

        if (strchr("=<>(),*", input[pos]) != NULL) {
            list->tokens[list->count].kind = TOKEN_SYMBOL;
            list->tokens[list->count].text[0] = input[pos];
            list->tokens[list->count].text[1] = '\0';
            list->count++;
            pos++;
            continue;
        }

        if (isdigit((unsigned char)input[pos])) {
            char num[MAX_STR_LEN] = {0};
            size_t num_pos = 0;
            while (pos < len && isdigit((unsigned char)input[pos]) && num_pos < MAX_STR_LEN - 1) {
                num[num_pos++] = input[pos++];
            }
            num[num_pos] = '\0';
            list->tokens[list->count].kind = TOKEN_NUMBER;
            strcpy(list->tokens[list->count].text, num);
            list->count++;
            continue;
        }

        if (isalpha((unsigned char)input[pos]) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\') {
            char ident[MAX_STR_LEN] = {0};
            size_t ident_pos = 0;
            while (pos < len && (isalnum((unsigned char)input[pos]) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\') && ident_pos < MAX_STR_LEN - 1) {
                ident[ident_pos++] = input[pos++];
            }
            ident[ident_pos] = '\0';
            list->tokens[list->count].kind = TOKEN_IDENTIFIER;
            strcpy(list->tokens[list->count].text, ident);
            list->count++;
            continue;
        }

        pos++;
    }

    list->tokens[list->count].kind = TOKEN_END;
    list->tokens[list->count].text[0] = '\0';
}

static Token peek_token(const TokenList* list) {
    return list->tokens[list->cursor];
}

static Token advance_token(TokenList* list) {
    return list->tokens[list->cursor++];
}

static bool match_token(TokenList* list, const char* text) {
    if (strcasecmp_custom(peek_token(list).text, text) == 0) {
        list->cursor++;
        return true;
    }
    return false;
}

/* Parses a single comparison: <column> <op> <value> */
static PrepareResult parse_comparison(TokenList* list, const Schema* schema, Expr** out) {
    Token col = advance_token(list);
    Token op = advance_token(list);
    Token val = advance_token(list);

    if (col.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

    int col_idx = schema_find_column(schema, col.text);
    if (col_idx < 0) return PREPARE_SYNTAX_ERROR;

    Expr* cmp = (Expr*)calloc(1, sizeof(Expr));
    cmp->type = EXPR_COMPARISON;
    strncpy(cmp->column, schema->columns[col_idx].name, MAX_NAME_LEN - 1);

    if (strcmp(op.text, "=") == 0) cmp->op = WHERE_OP_EQ;
    else if (strcmp(op.text, "!=") == 0 || strcmp(op.text, "<>") == 0) cmp->op = WHERE_OP_NEQ;
    else if (strcmp(op.text, ">=") == 0) cmp->op = WHERE_OP_GE;
    else if (strcmp(op.text, "<=") == 0) cmp->op = WHERE_OP_LE;
    else if (strcmp(op.text, ">") == 0) cmp->op = WHERE_OP_GT;
    else if (strcmp(op.text, "<") == 0) cmp->op = WHERE_OP_LT;
    else {
        free(cmp);
        return PREPARE_SYNTAX_ERROR;
    }

    if (schema->columns[col_idx].type == DATA_TYPE_INT) {
        cmp->value.is_string = false;
        char* endptr;
        long int_v = strtol(val.text, &endptr, 10);
        if (*endptr != '\0') {
            free(cmp);
            return PREPARE_SYNTAX_ERROR;
        }
        if (schema->columns[col_idx].is_primary_key && int_v < 0) {
            free(cmp);
            return PREPARE_NEGATIVE_ID;
        }
        cmp->value.int_value = (uint32_t)int_v;
    } else {
        cmp->value.is_string = true;
        strncpy(cmp->value.str_value, val.text, MAX_STR_LEN - 1);
    }

    *out = cmp;
    return PREPARE_SUCCESS;
}

static PrepareResult parse_expr(TokenList* list, const Schema* schema, Expr** out);

static PrepareResult parse_primary(TokenList* list, const Schema* schema, Expr** out) {
    if (peek_token(list).kind == TOKEN_SYMBOL && strcmp(peek_token(list).text, "(") == 0) {
        advance_token(list); /* consume '(' */
        PrepareResult res = parse_expr(list, schema, out);
        if (res != PREPARE_SUCCESS) return res;
        if (peek_token(list).kind != TOKEN_SYMBOL || strcmp(peek_token(list).text, ")") != 0) {
            free_expr(*out);
            *out = NULL;
            return PREPARE_SYNTAX_ERROR;
        }
        advance_token(list); /* consume ')' */
        return PREPARE_SUCCESS;
    }
    return parse_comparison(list, schema, out);
}

static PrepareResult parse_and(TokenList* list, const Schema* schema, Expr** out) {
    Expr* left = NULL;
    PrepareResult res = parse_primary(list, schema, &left);
    if (res != PREPARE_SUCCESS) return res;

    while (match_token(list, "AND")) {
        Expr* right = NULL;
        res = parse_primary(list, schema, &right);
        if (res != PREPARE_SUCCESS) {
            free_expr(left);
            return res;
        }
        Expr* parent = (Expr*)calloc(1, sizeof(Expr));
        parent->type = EXPR_LOGICAL;
        parent->log_op = LOGICAL_AND;
        parent->left = left;
        parent->right = right;
        left = parent;
    }
    *out = left;
    return PREPARE_SUCCESS;
}

static PrepareResult parse_or(TokenList* list, const Schema* schema, Expr** out) {
    Expr* left = NULL;
    PrepareResult res = parse_and(list, schema, &left);
    if (res != PREPARE_SUCCESS) return res;

    while (match_token(list, "OR")) {
        Expr* right = NULL;
        res = parse_and(list, schema, &right);
        if (res != PREPARE_SUCCESS) {
            free_expr(left);
            return res;
        }
        Expr* parent = (Expr*)calloc(1, sizeof(Expr));
        parent->type = EXPR_LOGICAL;
        parent->log_op = LOGICAL_OR;
        parent->left = left;
        parent->right = right;
        left = parent;
    }
    *out = left;
    return PREPARE_SUCCESS;
}

static PrepareResult parse_expr(TokenList* list, const Schema* schema, Expr** out) {
    return parse_or(list, schema, out);
}

static PrepareResult parse_where_clause(TokenList* list, const Schema* schema, Statement* statement) {
    if (!match_token(list, "WHERE")) return PREPARE_SUCCESS;
    return parse_expr(list, schema, &statement->where);
}

PrepareResult prepare_statement(TokenList* list, const Schema* schema, Statement* statement) {
    memset(statement, 0, sizeof(Statement));
    if (list->count == 0 || peek_token(list).kind == TOKEN_END) {
        return PREPARE_SYNTAX_ERROR;
    }

    Token first = peek_token(list);

    /* CREATE TABLE <name> (col1 INT PRIMARY KEY, col2 VARCHAR(32), ...) */
    if (strcasecmp_custom(first.text, "create") == 0) {
        advance_token(list);
        if (!match_token(list, "table")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        statement->type = STATEMENT_CREATE;
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        if (strcmp(peek_token(list).text, "(") != 0) return PREPARE_SYNTAX_ERROR;
        advance_token(list); /* consume '(' */

        memset(&statement->created_schema, 0, sizeof(Schema));
        strncpy(statement->created_schema.table_name, tbl.text, MAX_NAME_LEN - 1);

        while (list->cursor < list->count && strcmp(peek_token(list).text, ")") != 0) {
            Token col_name = advance_token(list);
            if (col_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

            Token col_type = advance_token(list);
            DataType dt = DATA_TYPE_INT;
            uint32_t len = 0;

            if (strcasecmp_custom(col_type.text, "int") == 0 || strcasecmp_custom(col_type.text, "integer") == 0) {
                dt = DATA_TYPE_INT;
            } else if (strcasecmp_custom(col_type.text, "varchar") == 0 || strcasecmp_custom(col_type.text, "string") == 0 || strcasecmp_custom(col_type.text, "char") == 0) {
                dt = DATA_TYPE_VARCHAR;
                len = 32;
                if (strcmp(peek_token(list).text, "(") == 0) {
                    advance_token(list);
                    Token len_tok = advance_token(list);
                    len = (uint32_t)strtoul(len_tok.text, NULL, 10);
                    if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
                }
            } else {
                return PREPARE_SYNTAX_ERROR;
            }

            bool is_pk = false;
            if (strcasecmp_custom(peek_token(list).text, "primary") == 0) {
                advance_token(list);
                if (strcasecmp_custom(peek_token(list).text, "key") == 0) advance_token(list);
                is_pk = true;
            }

            schema_add_column(&statement->created_schema, col_name.text, dt, len, is_pk);

            if (strcmp(peek_token(list).text, ",") == 0) {
                advance_token(list);
            }
        }

        if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
        return PREPARE_SUCCESS;
    }

    if (strcasecmp_custom(first.text, "begin") == 0 || strcasecmp_custom(first.text, "start") == 0) {
        advance_token(list);
        if (strcasecmp_custom(first.text, "start") == 0) match_token(list, "transaction");
        statement->type = STATEMENT_BEGIN;
        return PREPARE_SUCCESS;
    }

    if (strcasecmp_custom(first.text, "commit") == 0) {
        advance_token(list);
        statement->type = STATEMENT_COMMIT;
        return PREPARE_SUCCESS;
    }

    if (strcasecmp_custom(first.text, "rollback") == 0) {
        advance_token(list);
        statement->type = STATEMENT_ROLLBACK;
        return PREPARE_SUCCESS;
    }

    /* INSERT INTO <table> VALUES (val1, val2, ...) */
    if (strcasecmp_custom(first.text, "insert") == 0) {
        advance_token(list);
        match_token(list, "into");
        advance_token(list); /* consume table name */

        if (strcmp(peek_token(list).text, "(") == 0) {
            while (list->cursor < list->count && strcmp(peek_token(list).text, ")") != 0) advance_token(list);
            if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
        }

        if (!match_token(list, "values")) return PREPARE_SYNTAX_ERROR;
        if (strcmp(peek_token(list).text, "(") == 0) advance_token(list);

        statement->type = STATEMENT_INSERT;
        statement->row_to_insert.num_values = schema->num_columns;

        for (uint32_t i = 0; i < schema->num_columns; ++i) {
            Token val_tok = advance_token(list);
            if (strcmp(peek_token(list).text, ",") == 0) advance_token(list);

            Value* v = &statement->row_to_insert.values[i];
            v->type = schema->columns[i].type;

            if (schema->columns[i].type == DATA_TYPE_INT) {
                char* endptr;
                long int_v = strtol(val_tok.text, &endptr, 10);
                if (*endptr != '\0') return PREPARE_SYNTAX_ERROR;
                if (schema->columns[i].is_primary_key && int_v < 0) return PREPARE_NEGATIVE_ID;
                v->int_val = (int32_t)int_v;
            } else {
                if (strlen(val_tok.text) > schema->columns[i].length) return PREPARE_STRING_TOO_LONG;
                strncpy(v->str_val, val_tok.text, MAX_STR_LEN - 1);
            }
        }

        if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
        return PREPARE_SUCCESS;
    }

    /* SELECT [* | COUNT(*)] FROM <table> [WHERE <expr>] */
    if (strcasecmp_custom(first.text, "select") == 0) {
        advance_token(list);
        statement->type = STATEMENT_SELECT;

        if (strcasecmp_custom(peek_token(list).text, "count") == 0) {
            advance_token(list);
            if (strcmp(peek_token(list).text, "(") == 0) advance_token(list);
            if (strcmp(peek_token(list).text, "*") == 0) advance_token(list);
            if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
            statement->is_count = true;
        } else if (strcmp(peek_token(list).text, "*") == 0) {
            advance_token(list);
        }

        if (match_token(list, "from")) {
            advance_token(list);
        }

        return parse_where_clause(list, schema, statement);
    }

    /* UPDATE <table> SET col1 = val1 [, col2 = val2] [WHERE <expr>] */
    if (strcasecmp_custom(first.text, "update") == 0) {
        advance_token(list);
        statement->type = STATEMENT_UPDATE;
        statement->is_set_update = true;

        advance_token(list); /* table name */
        match_token(list, "set");

        while (list->cursor < list->count && strcasecmp_custom(peek_token(list).text, "where") != 0 && strcmp(peek_token(list).text, ";") != 0) {
            if (strcmp(peek_token(list).text, ",") == 0) {
                advance_token(list);
                continue;
            }

            Token col = advance_token(list);
            if (!match_token(list, "=")) return PREPARE_SYNTAX_ERROR;
            Token val = advance_token(list);

            if (statement->num_update_assignments < MAX_ASSIGNMENTS) {
                UpdateAssignment* assign = &statement->update_assignments[statement->num_update_assignments++];
                strncpy(assign->column_name, col.text, MAX_NAME_LEN - 1);
                strncpy(assign->value_text, val.text, MAX_STR_LEN - 1);
            }
        }

        return parse_where_clause(list, schema, statement);
    }

    /* DELETE FROM <table> [WHERE <expr>] */
    if (strcasecmp_custom(first.text, "delete") == 0) {
        advance_token(list);
        statement->type = STATEMENT_DELETE;

        if (match_token(list, "from")) {
            advance_token(list);
        }

        return parse_where_clause(list, schema, statement);
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
}
