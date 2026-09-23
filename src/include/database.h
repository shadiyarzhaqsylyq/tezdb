#ifndef DB_DATABASE_H
#define DB_DATABASE_H

#include "common.h"
#include "pager.h"
#include "schema.h"
#include "btree.h"

// Catalog entry (lives on a fixed page)
struct TableCatalogEntry {
    bool in_use;
    Schema schema;
    uint32_t root_page_num;
};

// Database-wide state
struct Database {
    Pager* pager;
    TableCatalogEntry tables[MAX_TABLES];
    uint32_t num_tables;

    uint32_t free_pages[TABLE_MAX_PAGES];
    uint32_t num_free_pages;

    bool in_transaction;
    uint32_t tx_original_num_pages;
    void* tx_backup[TABLE_MAX_PAGES];
    bool tx_was_cached[TABLE_MAX_PAGES];
};

typedef struct DbHeader {
    uint32_t magic;
    uint32_t num_tables;
} DbHeader;

typedef struct FreeListPage {
    uint32_t count;
    uint32_t pages[TABLE_MAX_PAGES];
} FreeListPage;

// Persistence helpers
void persist_catalog_entry(Database* db, uint32_t slot);
void persist_header(Database* db);
void persist_free_list(Database* db);
void reload_catalog_from_disk(Database* db);
void reload_free_list_from_disk(Database* db);

// Page allocation
uint32_t allocate_page(Database* db);
void     free_page(Database* db, uint32_t page_num);
void     collect_table_pages(Database* db, uint32_t page_num,
                             uint32_t* out_pages, uint32_t* out_count);

// Table lookup
int database_find_table_index(const Database* db, const char* name);
const TableCatalogEntry* database_find_table(const Database* db, const char* name);

// Lifecycle
Database* database_open(const char* filename);
void      database_close(Database* db);

// Transactions
void tx_free_backups(Database* db);

#endif /* DB_DATABASE_H */
