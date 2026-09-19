#pragma once

#include "Types.hpp"
#include "AST.hpp"
#include <string>
#include <vector>
#include <memory>
#include <optional>

enum class PrepareResult {
    Success,
    NegativeId,
    StringTooLong,
    SyntaxError,
    UnrecognizedStatement
};

enum class StatementType {
    Insert,
    Select,
    Update,
    Delete,
    Create,
    Begin,
    Commit,
    Rollback
};

struct UpdateAssignment {
    std::string column_name;
    std::string value_text;
};

struct Statement {
    StatementType type;
    DynamicRow row_to_insert;
    std::string table_name;
    Schema created_schema;
    std::unique_ptr<Expr> where_clause;
    bool is_count{false};
    std::vector<UpdateAssignment> update_assignments;
};

enum class TokenKind {
    Identifier,
    StringLiteral,
    Number,
    Symbol,
    End
};

struct Token {
    TokenKind kind{TokenKind::End};
    std::string text;
};

class Parser {
public:
    static std::vector<Token> tokenize(const std::string& input);
    static PrepareResult parse(const std::vector<Token>& tokens, const Schema& schema, Statement& statement);

private:
    static std::unique_ptr<Expr> parse_expr(const std::vector<Token>& tokens, size_t& cursor, const Schema& schema);
    static std::unique_ptr<Expr> parse_or(const std::vector<Token>& tokens, size_t& cursor, const Schema& schema);
    static std::unique_ptr<Expr> parse_and(const std::vector<Token>& tokens, size_t& cursor, const Schema& schema);
    static std::unique_ptr<Expr> parse_primary(const std::vector<Token>& tokens, size_t& cursor, const Schema& schema);
    static std::unique_ptr<Expr> parse_comparison(const std::vector<Token>& tokens, size_t& cursor, const Schema& schema);

    static bool match_token(const std::vector<Token>& tokens, size_t& cursor, const std::string& text);
    static bool strcasecmp(const std::string& a, const std::string& b);
};
