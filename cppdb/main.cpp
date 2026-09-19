#include "Engine.hpp"
#include <iostream>
#include <string>

void print_help() {
    std::cout << "SQL Commands:\n"
              << "  CREATE TABLE <name> (<pk_col> INT PRIMARY KEY, <col2> VARCHAR(32), <col3> INT, ...);\n"
              << "  INSERT INTO <name> VALUES (<val1>, '<val2>', ...);\n"
              << "  SELECT * FROM <name> [WHERE <expr>];\n"
              << "  SELECT COUNT(*) FROM <name> [WHERE <expr>];\n"
              << "  UPDATE <name> SET <col> = <val> WHERE <expr>;\n"
              << "  DELETE FROM <name> WHERE <expr>;\n"
              << "  (WHERE supports =, !=, <>, <, >, <=, >=, AND, OR, and parentheses ())\n"
              << "  BEGIN; | COMMIT; | ROLLBACK;\n"
              << "Meta commands:\n"
              << "  \\q or .exit      quit the shell\n"
              << "  \\d or .btree     print the B+tree structure\n"
              << "  \\c or .constants print page size constants\n"
              << "  \\? or .help      show this message\n";
}

bool handle_meta_command(const std::string& input, Table& table) {
    if (input == ".exit" || input == "\\q") {
        std::exit(EXIT_SUCCESS);
    } else if (input == ".btree" || input == "\\d") {
        std::cout << "Tree:\n";
        table.print_tree(table.root_page_num(), 0);
        return true;
    } else if (input == ".constants" || input == "\\c") {
        std::cout << "Constants:\n";
        table.print_constants();
        return true;
    } else if (input == ".help" || input == "\\?") {
        print_help();
        return true;
    }
    return false;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Must supply a database filename.\n";
        return EXIT_FAILURE;
    }

    try {
        Table table(argv[1]);
        std::string line;

        while (true) {
            std::cout << "db=# " << std::flush;
            if (!std::getline(std::cin, line)) break;
            if (line.empty()) continue;

            if (line[0] == '.' || line[0] == '\\') {
                if (!handle_meta_command(line, table)) {
                    std::cout << "Unrecognized command '" << line << "'\n";
                }
                continue;
            }

            auto tokens = Parser::tokenize(line);
            Statement stmt;
            PrepareResult prep_res = Parser::parse(tokens, table.schema(), stmt);

            switch (prep_res) {
                case PrepareResult::Success:
                    break;
                case PrepareResult::NegativeId:
                    std::cout << "ID must be positive.\n";
                    continue;
                case PrepareResult::StringTooLong:
                    std::cout << "String is too long for column budget.\n";
                    continue;
                case PrepareResult::SyntaxError:
                    std::cout << "Syntax error. Could not parse statement.\n";
                    continue;
                case PrepareResult::UnrecognizedStatement:
                    std::cout << "Unrecognized keyword at start of '" << line << "'.\n";
                    continue;
            }

            if (!table.schema().has_schema &&
                (stmt.type == StatementType::Insert || stmt.type == StatementType::Select ||
                 stmt.type == StatementType::Update || stmt.type == StatementType::Delete)) {
                std::cout << "Error: No table schema found. Please run CREATE TABLE first.\n";
                continue;
            }

            ExecuteResult exec_res = table.execute(stmt);
            switch (exec_res) {
                case ExecuteResult::Success:
                    if (stmt.type == StatementType::Insert) {
                        std::cout << "INSERT 0 1\n";
                    } else if (stmt.type == StatementType::Begin) {
                        std::cout << "BEGIN\n";
                    } else if (stmt.type == StatementType::Commit) {
                        std::cout << "COMMIT\n";
                    } else if (stmt.type == StatementType::Rollback) {
                        std::cout << "ROLLBACK\n";
                    }
                    break;
                case ExecuteResult::DuplicateKey:
                    std::cout << "Error: Duplicate key.\n";
                    break;
                case ExecuteResult::NotFound:
                    std::cout << "Error: row not found.\n";
                    break;
                case ExecuteResult::TxAlreadyActive:
                    std::cout << "Error: a transaction is already active.\n";
                    break;
                case ExecuteResult::NoActiveTx:
                    std::cout << "Error: no active transaction.\n";
                    break;
                case ExecuteResult::TableFull:
                    std::cout << "Error: table is full (max " << TABLE_MAX_PAGES << " pages).\n";
                    break;
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Fatal database error: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
