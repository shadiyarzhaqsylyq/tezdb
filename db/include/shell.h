#ifndef SHELL_H
#define SHELL_H

#include "common.h"
#include "btree.h"

void print_help(void);
void print_constants(const Table* table);
MetaCommandResult do_meta_command(const char* input, Table* table);
bool preprocess_input(char** line_ptr);
void execute_line(char* raw_line, Table* table);
void run_stream(FILE* stream, Table* table);

#endif /* SHELL_H */
