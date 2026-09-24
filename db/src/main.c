#include "common.h"
#include "btree.h"
#include "shell.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <db_filename> [script.sql]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char* filename = argv[1];
    Table* table = table_open(filename);

    if (argc >= 3) {
        /* Mode 1: Run SQL script file provided as CLI argument */
        const char* script_path = argv[2];
        FILE* fp = fopen(script_path, "r");
        if (!fp) {
            fprintf(stderr, "Error: Could not open script file '%s': %s\n", script_path, strerror(errno));
            table_close(table);
            exit(EXIT_FAILURE);
        }
        run_stream(fp, table);
        fclose(fp);
    } else {
        /* Mode 2 & 3: Read from stdin (interactive shell or piped/redirected < init.sql) */
        run_stream(stdin, table);
    }

    table_close(table);
    return 0;
}
