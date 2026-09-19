#include "Parser.hpp"
#include <cctype>

bool Parser::strcasecmp(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), [](char c1, char c2) {
        return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2));
    });
}

std::vector<Token> Parser::tokenize(const std::string& input) {
    std::vector<Token> tokens;
    size_t pos = 0;
    const size_t len = input.length();

    while (pos < len) {
        if (std::isspace(static_cast<unsigned char>(input[pos]))) {
            pos++;
            continue;
        }

        if (input[pos] == ';') {
            tokens.push_back({TokenKind::Symbol, ";"});
            pos++;
            continue;
        }

        if (input[pos] == '\'') {
            std::string str;
            pos++;
            while (pos < len && input[pos] != '\'') {
                str += input[pos++];
            }
            if (pos < len && input[pos] == '\'') pos++;
            tokens.push_back({TokenKind::StringLiteral, std::move(str)});
            continue;
        }

        if (pos + 1 < len) {
            std::string op2 = input.substr(pos, 2);
            if (op2 == "<=" || op2 == ">=" || op2 == "!=" || op2 == "<>") {
                tokens.push_back({TokenKind::Symbol, std::move(op2)});
                pos += 2;
                continue;
            }
        }

        if (std::string("=<>(),*").find(input[pos]) != std::string::npos) {
            tokens.push_back({TokenKind::Symbol, std::string(1, input[pos])});
            pos++;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(input[pos]))) {
            std::string num;
            while (pos < len && std::isdigit(static_cast<unsigned char>(input[pos]))) {
                num += input[pos++];
            }
            tokens.push_back({TokenKind::Number, std::move(num)});
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(input[pos])) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\') {
            std::string ident;
            while (pos < len && (std::isalnum(static_cast<unsigned char>(input[pos])) || 
                   input[pos] == '_' || input[pos] == '.' || input[pos] == '\\')) {
                ident += input[pos++];
            }
            tokens.push_back({TokenKind::Identifier, std::move(ident)});
            continue;
        }

        pos++;
    }

    tokens.push_back({TokenKind::End, ""});
    return tokens;
}

bool Parser::match_token(const std::vector<Token>& tokens, size_t& cursor, const std::string& text) {
    if (cursor < tokens.size() && strcasecmp(tokens[cursor].text, text)) {
        cursor++;
        return true;
    }
    return false;
}

std::unique_ptr<Expr> Parser::parse_comparison(const std::vector<Token>& tokens, size_t& cursor, const Schema& schema) {
    if (cursor + 3 > tokens.size()) return nullptr;

    const Token& col = tokens[cursor++];
    const Token& op = tokens[cursor++];
    const Token& val = tokens[cursor++];

    if (col.kind != TokenKind::Identifier) return nullptr;

    int col_idx = schema.find_column(col.text);
    if (col_idx < 0) return nullptr;

    WhereOp where_op;
    if (op.text == "=") where_op = WhereOp::Eq;
    else if (op.text == "!=" || op.text == "<>") where_op = WhereOp::Neq;
    else if (op.text == ">=") where_op = WhereOp::Ge;
    else if (op.text == "<=") where_op = WhereOp::Le;
    else if (op.text == ">")  where_op = WhereOp::Gt;
    else if (op.text == "<")  where_op = WhereOp::Lt;
    else return nullptr;

    Value v;
    if (schema.columns[col_idx].type == DataType::Int) {
        try {
            int32_t parsed = std::stoi(val.text);
            v = parsed;
        } catch (...) {
            return nullptr;
        }
    } else {
        v = val.text;
    }

    return std::make_unique<ComparisonExpr>(schema.columns[col_idx].name, where_op, std::move(v));
}

std::unique_ptr<Expr> Parser::parse_primary(const std::vector<Token>& tokens, size_t& cursor, const Schema& schema) {
    if (cursor < tokens.size() && tokens[cursor].kind == TokenKind::Symbol && tokens[cursor].text == "(") {
        cursor++;
        auto expr = parse_expr(tokens, cursor, schema);
        if (!expr) return nullptr;
        if (cursor >= tokens.size() || tokens[cursor].text != ")") return nullptr;
        cursor++;
        return expr;
    }
    return parse_comparison(tokens, cursor, schema);
}

std::unique_ptr<Expr> Parser::parse_and(const std::vector<Token>& tokens, size_t& cursor, const Schema& schema) {
    auto left = parse_primary(tokens, cursor, schema);
    if (!left) return nullptr;

    while (match_token(tokens, cursor, "AND")) {
        auto right = parse_primary(tokens, cursor, schema);
        if (!right) return nullptr;
        left = std::make_unique<LogicalExpr>(LogicalOp::And, std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<Expr> Parser::parse_or(const std::vector<Token>& tokens, size_t& cursor, const Schema& schema) {
    auto left = parse_and(tokens, cursor, schema);
    if (!left) return nullptr;

    while (match_token(tokens, cursor, "OR")) {
        auto right = parse_and(tokens, cursor, schema);
        if (!right) return nullptr;
        left = std::make_unique<LogicalExpr>(LogicalOp::Or, std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<Expr> Parser::parse_expr(const std::vector<Token>& tokens, size_t& cursor, const Schema& schema) {
    return parse_or(tokens, cursor, schema);
}

PrepareResult Parser::parse(const std::vector<Token>& tokens, const Schema& schema, Statement& stmt) {
    if (tokens.empty() || tokens[0].kind == TokenKind::End) {
        return PrepareResult::SyntaxError;
    }

    size_t cursor = 0;
    const Token& first = tokens[cursor++];

    if (strcasecmp(first.text, "create")) {
        if (!match_token(tokens, cursor, "table")) return PrepareResult::SyntaxError;
        if (cursor >= tokens.size()) return PrepareResult::SyntaxError;

        stmt.type = StatementType::Create;
        stmt.table_name = tokens[cursor++].text;

        if (cursor >= tokens.size() || tokens[cursor++].text != "(") return PrepareResult::SyntaxError;

        stmt.created_schema = Schema{};
        stmt.created_schema.table_name = stmt.table_name;

        while (cursor < tokens.size() && tokens[cursor].text != ")") {
            std::string col_name = tokens[cursor++].text;
            if (cursor >= tokens.size()) return PrepareResult::SyntaxError;

            std::string col_type = tokens[cursor++].text;
            DataType dt = DataType::Int;
            uint32_t len = 0;

            if (strcasecmp(col_type, "int") || strcasecmp(col_type, "integer")) {
                dt = DataType::Int;
            } else if (strcasecmp(col_type, "varchar") || strcasecmp(col_type, "string") || strcasecmp(col_type, "char")) {
                dt = DataType::VarChar;
                len = 32;
                if (cursor < tokens.size() && tokens[cursor].text == "(") {
                    cursor++;
                    len = static_cast<uint32_t>(std::stoul(tokens[cursor++].text));
                    if (cursor < tokens.size() && tokens[cursor].text == ")") cursor++;
                }
            } else {
                return PrepareResult::SyntaxError;
            }

            bool is_pk = false;
            if (cursor < tokens.size() && strcasecmp(tokens[cursor].text, "primary")) {
                cursor++;
                if (cursor < tokens.size() && strcasecmp(tokens[cursor].text, "key")) cursor++;
                is_pk = true;
            }

            stmt.created_schema.add_column(col_name, dt, len, is_pk);

            if (cursor < tokens.size() && tokens[cursor].text == ",") {
                cursor++;
            }
        }
        return PrepareResult::Success;
    }

    if (strcasecmp(first.text, "begin") || strcasecmp(first.text, "start")) {
        stmt.type = StatementType::Begin;
        return PrepareResult::Success;
    }
    if (strcasecmp(first.text, "commit")) {
        stmt.type = StatementType::Commit;
        return PrepareResult::Success;
    }
    if (strcasecmp(first.text, "rollback")) {
        stmt.type = StatementType::Rollback;
        return PrepareResult::Success;
    }

    if (strcasecmp(first.text, "insert")) {
        match_token(tokens, cursor, "into");
        if (cursor < tokens.size()) cursor++; // table name

        if (cursor < tokens.size() && tokens[cursor].text == "(") {
            while (cursor < tokens.size() && tokens[cursor].text != ")") cursor++;
            if (cursor < tokens.size() && tokens[cursor].text == ")") cursor++;
        }

        if (!match_token(tokens, cursor, "values")) return PrepareResult::SyntaxError;
        if (cursor < tokens.size() && tokens[cursor].text == "(") cursor++;

        stmt.type = StatementType::Insert;

        for (const auto& col : schema.columns) {
            if (cursor >= tokens.size()) return PrepareResult::SyntaxError;
            const auto& tok = tokens[cursor++];
            if (cursor < tokens.size() && tokens[cursor].text == ",") cursor++;

            if (col.type == DataType::Int) {
                try {
                    int32_t val = std::stoi(tok.text);
                    if (col.is_primary_key && val < 0) return PrepareResult::NegativeId;
                    stmt.row_to_insert.values.emplace_back(val);
                } catch (...) {
                    return PrepareResult::SyntaxError;
                }
            } else {
                if (tok.text.length() > col.length) return PrepareResult::StringTooLong;
                stmt.row_to_insert.values.emplace_back(tok.text);
            }
        }
        return PrepareResult::Success;
    }

    if (strcasecmp(first.text, "select")) {
        stmt.type = StatementType::Select;
        if (cursor < tokens.size() && strcasecmp(tokens[cursor].text, "count")) {
            cursor++;
            if (cursor < tokens.size() && tokens[cursor].text == "(") cursor++;
            if (cursor < tokens.size() && tokens[cursor].text == "*") cursor++;
            if (cursor < tokens.size() && tokens[cursor].text == ")") cursor++;
            stmt.is_count = true;
        } else if (cursor < tokens.size() && tokens[cursor].text == "*") {
            cursor++;
        }

        if (match_token(tokens, cursor, "from") && cursor < tokens.size()) {
            cursor++;
        }

        if (match_token(tokens, cursor, "where")) {
            stmt.where_clause = parse_expr(tokens, cursor, schema);
        }
        return PrepareResult::Success;
    }

    if (strcasecmp(first.text, "update")) {
        stmt.type = StatementType::Update;
        if (cursor < tokens.size()) cursor++; // Table name
        if (!match_token(tokens, cursor, "set")) return PrepareResult::SyntaxError;

        while (cursor < tokens.size() && !strcasecmp(tokens[cursor].text, "where") && tokens[cursor].text != ";") {
            if (tokens[cursor].text == ",") {
                cursor++;
                continue;
            }
            std::string col = tokens[cursor++].text;
            if (!match_token(tokens, cursor, "=") || cursor >= tokens.size()) {
                return PrepareResult::SyntaxError;
            }
            std::string val = tokens[cursor++].text;
            stmt.update_assignments.push_back({std::move(col), std::move(val)});
        }

        if (match_token(tokens, cursor, "where")) {
            stmt.where_clause = parse_expr(tokens, cursor, schema);
        }
        return PrepareResult::Success;
    }

    if (strcasecmp(first.text, "delete")) {
        stmt.type = StatementType::Delete;
        if (match_token(tokens, cursor, "from") && cursor < tokens.size()) {
            cursor++;
        }
        if (match_token(tokens, cursor, "where")) {
            stmt.where_clause = parse_expr(tokens, cursor, schema);
        }
        return PrepareResult::Success;
    }

    return PrepareResult::UnrecognizedStatement;
}
