#ifndef DB_EXECUTOR_H
#define DB_EXECUTOR_H

#include "common.h"
#include "database.h"
#include "btree.h"
#include "schema.h"


ExecuteResult execute_statement(Statement* statement, Database* db);

#endif /* DB_EXECUTOR_H */
