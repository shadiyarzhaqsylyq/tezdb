#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "common.h"
#include "schema.h"
#include "row.h"
#include "expr.h"
#include "btree.h"
#include "parser.h"

bool row_matches_where(const DynamicRow* row, const Statement* statement, const Schema* schema);
ExecuteResult execute_insert(Statement* statement, Table* table);
ExecuteResult execute_update(Statement* statement, Table* table);
ExecuteResult execute_delete(Statement* statement, Table* table);
ExecuteResult execute_select(Statement* statement, Table* table);
ExecuteResult execute_begin(Table* table);
ExecuteResult execute_commit(Table* table);
ExecuteResult execute_rollback(Table* table);
ExecuteResult execute_statement(Statement* statement, Table* table);

#endif /* EXECUTOR_H */
