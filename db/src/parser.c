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

        if (isdigit((unsigned char)input[pos]) ||
            (input[pos] == '-' && pos + 1 < len && isdigit((unsigned char)input[pos + 1]))) {
            char num[MAX_STR_LEN] = {0};
            size_t num_pos = 0;
            if (input[pos] == '-') num[num_pos++] = input[pos++];
            while (pos < len && isdigit((unsigned char)input[pos]) && num_pos < MAX_STR_LEN - 1) {
                num[num_pos++] = input[pos++];
            }
            num[num_pos] = '\0';
            list->tokens[list->count].kind = TOKEN_NUMBER;
            strcpy(list->tokens[list->count].text, num);
            list->count++;
            continue;
        }

        if (strchr("=<>(),*-", input[pos]) != NULL) {
            list->tokens[list->count].kind = TOKEN_SYMBOL;
            list->tokens[list->count].text[0] = input[pos];
            list->tokens[list->count].text[1] = '\0';
            list->count++;
            pos++;
            continue;
        }

        if (isalpha((unsigned char)input[pos]) || input[pos] == '_' ||
            input[pos] == '.' || input[pos] == '\\') {
            char ident[MAX_STR_LEN] = {0};
            size_t ident_pos = 0;
            while (pos < len &&
                   (isalnum((unsigned char)input[pos]) || input[pos] == '_' ||
                    input[pos] == '.' || input[pos] == '\\') &&
                   ident_pos < MAX_STR_LEN - 1) {
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
        advance_token(list);
        PrepareResult res = parse_expr(list, schema, out);
        if (res != PREPARE_SUCCESS) return res;
        if (peek_token(list).kind != TOKEN_SYMBOL || strcmp(peek_token(list).text, ")") != 0) {
            free_expr(*out);
            *out = NULL;
            return PREPARE_SYNTAX_ERROR;
        }
        advance_token(list);
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

static PrepareResult parse_select_modifiers(TokenList* list, const Schema* schema, Statement* statement) {
    if (match_token(list, "order")) {
        if (!match_token(list, "by")) return PREPARE_SYNTAX_ERROR;
        Token col = advance_token(list);
        if (col.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
        if (schema_find_column(schema, col.text) < 0) return PREPARE_SYNTAX_ERROR;

        statement->has_order_by = true;
        strncpy(statement->order_by_column, col.text, MAX_NAME_LEN - 1);
        statement->order_by_desc = false;

        if (strcasecmp_custom(peek_token(list).text, "desc") == 0) {
            advance_token(list);
            statement->order_by_desc = true;
        } else if (strcasecmp_custom(peek_token(list).text, "asc") == 0) {
            advance_token(list);
        }
    }

    if (match_token(list, "limit")) {
        Token n = advance_token(list);
        if (n.kind != TOKEN_NUMBER) return PREPARE_SYNTAX_ERROR;
        statement->has_limit = true;
        statement->limit_count = (uint32_t)strtoul(n.text, NULL, 10);
    }

    if (match_token(list, "offset")) {
        Token n = advance_token(list);
        if (n.kind != TOKEN_NUMBER) return PREPARE_SYNTAX_ERROR;
        statement->has_offset = true;
        statement->offset_count = (uint32_t)strtoul(n.text, NULL, 10);
    }

    return PREPARE_SUCCESS;
}

static PrepareResult parse_column_type(TokenList* list, DataType* dt_out, uint32_t* len_out) {
    Token col_type = advance_token(list);
    if (strcasecmp_custom(col_type.text, "int") == 0 ||
        strcasecmp_custom(col_type.text, "integer") == 0) {
        *dt_out = DATA_TYPE_INT;
        *len_out = 0;
    } else if (strcasecmp_custom(col_type.text, "varchar") == 0 ||
               strcasecmp_custom(col_type.text, "string") == 0 ||
               strcasecmp_custom(col_type.text, "char") == 0) {
        *dt_out = DATA_TYPE_VARCHAR;
        *len_out = 32;
        if (strcmp(peek_token(list).text, "(") == 0) {
            advance_token(list);
            Token len_tok = advance_token(list);
            *len_out = (uint32_t)strtoul(len_tok.text, NULL, 10);
            if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
        }
    } else {
        return PREPARE_SYNTAX_ERROR;
    }
    return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(TokenList* list, const Database* db, Statement* statement) {
    memset(statement, 0, sizeof(Statement));
    if (list->count == 0 || peek_token(list).kind == TOKEN_END) {
        return PREPARE_SYNTAX_ERROR;
    }

    Token first = peek_token(list);

    // CREATE TABLE
    if (strcasecmp_custom(first.text, "create") == 0) {
        advance_token(list);
        if (!match_token(list, "table")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        statement->type = STATEMENT_CREATE;
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        if (strcmp(peek_token(list).text, "(") != 0) return PREPARE_SYNTAX_ERROR;
        advance_token(list);

        memset(&statement->created_schema, 0, sizeof(Schema));
        strncpy(statement->created_schema.table_name, tbl.text, MAX_NAME_LEN - 1);

        while (list->cursor < list->count && strcmp(peek_token(list).text, ")") != 0) {
            Token col_name = advance_token(list);
            if (col_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

            DataType dt;
            uint32_t len;
            PrepareResult type_res = parse_column_type(list, &dt, &len);
            if (type_res != PREPARE_SUCCESS) return type_res;

            bool is_pk = false;
            if (strcasecmp_custom(peek_token(list).text, "primary") == 0) {
                advance_token(list);
                if (strcasecmp_custom(peek_token(list).text, "key") == 0) advance_token(list);
                is_pk = true;
            }

            schema_add_column(&statement->created_schema, col_name.text, dt, len, is_pk);
			

			// --- Parse REFERENCES <parent_table>(<parent_col>) ---
			if (strcasecmp_custom(peek_token(list).text, "references") == 0) {
				advance_token(list); // consume 'REFERENCES'
				Token parent_tbl = advance_token(list);
				
				char parent_col[MAX_NAME_LEN] = "id"; // default to "id" if omitted
				if (strcmp(peek_token(list).text, "(") == 0) {
					advance_token(list); // consume '('
					Token p_col = advance_token(list);
					strncpy(parent_col, p_col.text, MAX_NAME_LEN - 1);
					if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
				}
				
				Schema* s = &statement->created_schema;
				if (s->num_foreign_keys < MAX_FOREIGN_KEYS) {
					ForeignKey* fk = &s->foreign_keys[s->num_foreign_keys++];
					fk->in_use = true;
					strncpy(fk->col_name, col_name.text, MAX_NAME_LEN - 1);
					strncpy(fk->ref_table, parent_tbl.text, MAX_NAME_LEN - 1);
					strncpy(fk->ref_col, parent_col, MAX_NAME_LEN - 1);
				}
			}


            if (strcmp(peek_token(list).text, ",") == 0) advance_token(list);
        }

        if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
        return PREPARE_SUCCESS;
    }

    // DROP TABLE
    if (strcasecmp_custom(first.text, "drop") == 0) {
        advance_token(list);
        if (!match_token(list, "table")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        if (tbl.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
        statement->type = STATEMENT_DROP;
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);
        return PREPARE_SUCCESS;
    }

    // ALTER TABLE
    if (strcasecmp_custom(first.text, "alter") == 0) {
        advance_token(list);
        if (!match_token(list, "table")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        if (tbl.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

        statement->type = STATEMENT_ALTER;
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        if (match_token(list, "add")) {
            match_token(list, "column");
            Token col_name = advance_token(list);
            if (col_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

            DataType dt;
            uint32_t len;
            PrepareResult type_res = parse_column_type(list, &dt, &len);
            if (type_res != PREPARE_SUCCESS) return type_res;

            statement->alter_kind = ALTER_ADD_COLUMN;
            strncpy(statement->alter_column_name, col_name.text, MAX_NAME_LEN - 1);
            statement->alter_column_type = dt;
            statement->alter_column_length = len;
            return PREPARE_SUCCESS;
        }

        if (match_token(list, "drop")) {
            match_token(list, "column");
            Token col_name = advance_token(list);
            if (col_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
            statement->alter_kind = ALTER_DROP_COLUMN;
            strncpy(statement->alter_column_name, col_name.text, MAX_NAME_LEN - 1);
            return PREPARE_SUCCESS;
        }

        if (match_token(list, "rename")) {
            if (match_token(list, "column")) {
                Token old_name = advance_token(list);
                if (old_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
                if (!match_token(list, "to")) return PREPARE_SYNTAX_ERROR;
                Token new_name = advance_token(list);
                if (new_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

                statement->alter_kind = ALTER_RENAME_COLUMN;
                strncpy(statement->alter_column_name, old_name.text, MAX_NAME_LEN - 1);
                strncpy(statement->alter_new_name, new_name.text, MAX_NAME_LEN - 1);
                return PREPARE_SUCCESS;
            }
            if (!match_token(list, "to")) return PREPARE_SYNTAX_ERROR;
            Token new_name = advance_token(list);
            if (new_name.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;

            statement->alter_kind = ALTER_RENAME_TABLE;
            strncpy(statement->alter_new_name, new_name.text, MAX_NAME_LEN - 1);
            return PREPARE_SUCCESS;
        }

        return PREPARE_SYNTAX_ERROR;
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

    // INSERT
    if (strcasecmp_custom(first.text, "insert") == 0) {
        advance_token(list);
        match_token(list, "into");
        Token tbl = advance_token(list);
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        const TableCatalogEntry* entry = database_find_table(db, tbl.text);
        if (!entry) return PREPARE_NO_SUCH_TABLE;
        const Schema* schema = &entry->schema;

        if (strcmp(peek_token(list).text, "(") == 0) {
            while (list->cursor < list->count && strcmp(peek_token(list).text, ")") != 0)
                advance_token(list);
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

    // SELECT
    if (strcasecmp_custom(first.text, "select") == 0) {
        advance_token(list);
        statement->type = STATEMENT_SELECT;
        statement->agg_type = AGG_NONE;

        Token sel = peek_token(list);
        if (strcasecmp_custom(sel.text, "count") == 0) {
            advance_token(list);
            if (strcmp(peek_token(list).text, "(") == 0) advance_token(list);
            if (strcmp(peek_token(list).text, "*") == 0) advance_token(list);
            if (strcmp(peek_token(list).text, ")") == 0) advance_token(list);
            statement->agg_type = AGG_COUNT;
        } else if (strcasecmp_custom(sel.text, "sum") == 0 ||
                   strcasecmp_custom(sel.text, "avg") == 0 ||
                   strcasecmp_custom(sel.text, "min") == 0 ||
                   strcasecmp_custom(sel.text, "max") == 0) {
            AggregateType agg = AGG_SUM;
            if (strcasecmp_custom(sel.text, "avg") == 0) agg = AGG_AVG;
            else if (strcasecmp_custom(sel.text, "min") == 0) agg = AGG_MIN;
            else if (strcasecmp_custom(sel.text, "max") == 0) agg = AGG_MAX;

            advance_token(list);
            if (!match_token(list, "(")) return PREPARE_SYNTAX_ERROR;
            Token col_tok = advance_token(list);
            if (col_tok.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
            if (!match_token(list, ")")) return PREPARE_SYNTAX_ERROR;

            statement->agg_type = agg;
            strncpy(statement->agg_column, col_tok.text, MAX_NAME_LEN - 1);
        } else if (strcmp(sel.text, "*") == 0) {
            advance_token(list);
            statement->select_all = true;
        } else {
            statement->select_all = false;
            while (list->cursor < list->count &&
                   strcasecmp_custom(peek_token(list).text, "from") != 0) {
                Token col = advance_token(list);
                if (col.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
                if (statement->num_select_columns >= MAX_COLUMNS) return PREPARE_SYNTAX_ERROR;
                strncpy(statement->select_columns[statement->num_select_columns++],
                        col.text, MAX_NAME_LEN - 1);
                if (strcasecmp_custom(peek_token(list).text, "from") == 0) break;
                if (!match_token(list, ",")) return PREPARE_SYNTAX_ERROR;
            }
            if (statement->num_select_columns == 0) return PREPARE_SYNTAX_ERROR;
        }

        if (!match_token(list, "from")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        const TableCatalogEntry* left_entry = database_find_table(db, tbl.text);
        if (!left_entry) return PREPARE_NO_SUCH_TABLE;

        const Schema* effective_schema = &left_entry->schema;
        Schema combined_schema;

        match_token(list, "inner");
        if (match_token(list, "join")) {
            Token rtbl = advance_token(list);
            if (rtbl.kind != TOKEN_IDENTIFIER) return PREPARE_SYNTAX_ERROR;
            const TableCatalogEntry* right_entry = database_find_table(db, rtbl.text);
            if (!right_entry) {
                strncpy(statement->table_name, rtbl.text, MAX_NAME_LEN - 1);
                return PREPARE_NO_SUCH_TABLE;
            }

            if (!match_token(list, "on")) return PREPARE_SYNTAX_ERROR;

            Token c1 = advance_token(list);
            Token op_tok = advance_token(list);
            Token c2 = advance_token(list);
            if (c1.kind != TOKEN_IDENTIFIER || c2.kind != TOKEN_IDENTIFIER)
                return PREPARE_SYNTAX_ERROR;

            WhereOp join_op;
            if (strcmp(op_tok.text, "=") == 0) join_op = WHERE_OP_EQ;
            else if (strcmp(op_tok.text, "!=") == 0 || strcmp(op_tok.text, "<>") == 0)
                join_op = WHERE_OP_NEQ;
            else if (strcmp(op_tok.text, ">=") == 0) join_op = WHERE_OP_GE;
            else if (strcmp(op_tok.text, "<=") == 0) join_op = WHERE_OP_LE;
            else if (strcmp(op_tok.text, ">") == 0) join_op = WHERE_OP_GT;
            else if (strcmp(op_tok.text, "<") == 0) join_op = WHERE_OP_LT;
            else return PREPARE_SYNTAX_ERROR;

            int li = schema_find_column(&left_entry->schema, c1.text);
            int ri = schema_find_column(&right_entry->schema, c2.text);
            bool swapped = false;
            if (li < 0 || ri < 0) {
                int li2 = schema_find_column(&left_entry->schema, c2.text);
                int ri2 = schema_find_column(&right_entry->schema, c1.text);
                if (li2 < 0 || ri2 < 0) return PREPARE_SYNTAX_ERROR;
                li = li2;
                ri = ri2;
                swapped = true;
            }
            if (left_entry->schema.num_columns + right_entry->schema.num_columns > MAX_COLUMNS)
                return PREPARE_SYNTAX_ERROR;
            if (left_entry->schema.columns[li].type != right_entry->schema.columns[ri].type)
                return PREPARE_SYNTAX_ERROR;

            statement->has_join = true;
            strncpy(statement->join_table_name, rtbl.text, MAX_NAME_LEN - 1);
            strncpy(statement->join_left_column, left_entry->schema.columns[li].name, MAX_NAME_LEN - 1);
            strncpy(statement->join_right_column, right_entry->schema.columns[ri].name, MAX_NAME_LEN - 1);
            statement->join_op = swapped ? flip_op(join_op) : join_op;

            build_combined_schema(&combined_schema, &left_entry->schema, &right_entry->schema);
            effective_schema = &combined_schema;
        }

        if (!statement->select_all && statement->agg_type == AGG_NONE) {
            for (uint32_t i = 0; i < statement->num_select_columns; i++) {
                int col_idx = schema_find_column(effective_schema, statement->select_columns[i]);
                if (col_idx < 0) {
                    printf("Error: column '%s' not found.\n", statement->select_columns[i]);
                    return PREPARE_SYNTAX_ERROR;
                }
                statement->select_column_indices[i] = (uint32_t)col_idx;
            }
        }

        if (statement->agg_type == AGG_SUM || statement->agg_type == AGG_AVG ||
            statement->agg_type == AGG_MIN || statement->agg_type == AGG_MAX) {
            if (schema_find_column(effective_schema, statement->agg_column) < 0)
                return PREPARE_SYNTAX_ERROR;
        }

        PrepareResult where_res = parse_where_clause(list, effective_schema, statement);
        if (where_res != PREPARE_SUCCESS) return where_res;

        return parse_select_modifiers(list, effective_schema, statement);
    }

    // UPDATE
    if (strcasecmp_custom(first.text, "update") == 0) {
        advance_token(list);
        statement->type = STATEMENT_UPDATE;
        statement->is_set_update = true;

        Token tbl = advance_token(list);
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        const TableCatalogEntry* entry = database_find_table(db, tbl.text);
        if (!entry) return PREPARE_NO_SUCH_TABLE;

        match_token(list, "set");

        while (list->cursor < list->count &&
               strcasecmp_custom(peek_token(list).text, "where") != 0 &&
               strcmp(peek_token(list).text, ";") != 0) {
            if (strcmp(peek_token(list).text, ",") == 0) {
                advance_token(list);
                continue;
            }

            Token col = advance_token(list);
            if (!match_token(list, "=")) return PREPARE_SYNTAX_ERROR;
            Token val = advance_token(list);

            if (statement->num_update_assignments < MAX_ASSIGNMENTS) {
                UpdateAssignment* assign =
                    &statement->update_assignments[statement->num_update_assignments++];
                strncpy(assign->column_name, col.text, MAX_NAME_LEN - 1);
                strncpy(assign->value_text, val.text, MAX_STR_LEN - 1);
            }
        }

        return parse_where_clause(list, &entry->schema, statement);
    }

    // DELETE
    if (strcasecmp_custom(first.text, "delete") == 0) {
        advance_token(list);
        statement->type = STATEMENT_DELETE;

        if (!match_token(list, "from")) return PREPARE_SYNTAX_ERROR;
        Token tbl = advance_token(list);
        strncpy(statement->table_name, tbl.text, MAX_NAME_LEN - 1);

        const TableCatalogEntry* entry = database_find_table(db, tbl.text);
        if (!entry) return PREPARE_NO_SUCH_TABLE;

        return parse_where_clause(list, &entry->schema, statement);
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
}
