#include "database.h"

void persist_catalog_entry(Database* db, uint32_t slot) {
    TableCatalogEntry* page = (TableCatalogEntry*)pager_get_page(db->pager, CATALOG_START_PAGE + slot);
    *page = db->tables[slot];
}

void persist_header(Database* db) {
    DbHeader* header = (DbHeader*)pager_get_page(db->pager, 0);
    header->magic = SCHEMA_MAGIC;
    header->num_tables = db->num_tables;
}

void persist_free_list(Database* db) {
    FreeListPage* page = (FreeListPage*)pager_get_page(db->pager, FREE_LIST_PAGE);
    page->count = db->num_free_pages;
    memcpy(page->pages, db->free_pages, db->num_free_pages * sizeof(uint32_t));
}

void reload_catalog_from_disk(Database* db) {
    DbHeader* header = (DbHeader*)pager_get_page(db->pager, 0);
    db->num_tables = header->num_tables;
    for (uint32_t i = 0; i < MAX_TABLES; i++) {
        TableCatalogEntry* page = (TableCatalogEntry*)pager_get_page(db->pager, CATALOG_START_PAGE + i);
        db->tables[i] = *page;
    }
}

void reload_free_list_from_disk(Database* db) {
    FreeListPage* page = (FreeListPage*)pager_get_page(db->pager, FREE_LIST_PAGE);
    db->num_free_pages = page->count;
    memcpy(db->free_pages, page->pages, db->num_free_pages * sizeof(uint32_t));
}

uint32_t allocate_page(Database* db) {
    uint32_t page_num;
    if (db->num_free_pages > 0) {
        page_num = db->free_pages[--db->num_free_pages];
    } else {
        page_num = db->pager->num_pages;
    }
    persist_free_list(db);
    return page_num;
}

void free_page(Database* db, uint32_t page_num) {
    if (db->num_free_pages < TABLE_MAX_PAGES) {
        db->free_pages[db->num_free_pages++] = page_num;
    }
}

void collect_table_pages(Database* db, uint32_t page_num,
                         uint32_t* out_pages, uint32_t* out_count) {
    if (page_num == INVALID_PAGE_NUM) return;
    void* node = pager_get_page(db->pager, page_num);
    if (get_node_type(node) == NODE_INTERNAL) {
        uint32_t num_keys = *internal_node_num_keys(node);
        for (uint32_t i = 0; i < num_keys; i++) {
            collect_table_pages(db, *internal_node_child(node, i), out_pages, out_count);
        }
        collect_table_pages(db, *internal_node_right_child(node), out_pages, out_count);
    }
    if (*out_count < TABLE_MAX_PAGES) {
        out_pages[(*out_count)++] = page_num;
    }
}

int database_find_table_index(const Database* db, const char* name) {
    for (uint32_t i = 0; i < MAX_TABLES; i++) {
        if (db->tables[i].in_use &&
            strcasecmp_custom(db->tables[i].schema.table_name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const TableCatalogEntry* database_find_table(const Database* db, const char* name) {
    int idx = database_find_table_index(db, name);
    return idx < 0 ? NULL : &db->tables[idx];
}

void tx_free_backups(Database* db) {
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (db->tx_backup[i] != NULL) {
            free(db->tx_backup[i]);
            db->tx_backup[i] = NULL;
        }
    }
}

Database* database_open(const char* filename) {
    Database* db = (Database*)malloc(sizeof(Database));
    db->pager = pager_open(filename);
    db->in_transaction = false;
    db->tx_original_num_pages = 0;
    db->num_tables = 0;
    db->num_free_pages = 0;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        db->tx_backup[i] = NULL;
        db->tx_was_cached[i] = false;
    }
    for (uint32_t i = 0; i < MAX_TABLES; i++) {
        memset(&db->tables[i], 0, sizeof(TableCatalogEntry));
    }

    if (db->pager->num_pages == 0) {
        DbHeader* header = (DbHeader*)pager_get_page(db->pager, 0);
        header->magic = SCHEMA_MAGIC;
        header->num_tables = 0;

        for (uint32_t i = 0; i < MAX_TABLES; i++) {
            TableCatalogEntry* page =
                (TableCatalogEntry*)pager_get_page(db->pager, CATALOG_START_PAGE + i);
            memset(page, 0, sizeof(TableCatalogEntry));
        }

        FreeListPage* free_list =
            (FreeListPage*)pager_get_page(db->pager, FREE_LIST_PAGE);
        free_list->count = 0;
    } else {
        DbHeader* header = (DbHeader*)pager_get_page(db->pager, 0);
        if (header->magic == SCHEMA_MAGIC_V1_SINGLE_TABLE) {
            printf("Error: '%s' uses an older single-table format and cannot be opened by this version.\n",
                   filename);
            exit(EXIT_FAILURE);
        }
        if (header->magic == SCHEMA_MAGIC_V2_NO_FREELIST) {
            printf("Error: '%s' uses an older multi-table format without page reclamation and cannot be opened by this version.\n",
                   filename);
            exit(EXIT_FAILURE);
        }
        if (header->magic != SCHEMA_MAGIC) {
            printf("Db file has an unrecognized header. Corrupt file.\n");
            exit(EXIT_FAILURE);
        }
        reload_catalog_from_disk(db);
        reload_free_list_from_disk(db);
    }
    return db;
}

void database_close(Database* db) {
    tx_free_backups(db);
    for (uint32_t i = 0; i < db->pager->num_pages; i++) {
        if (db->pager->pages[i] == NULL) continue;
        pager_flush(db->pager, i);
        free(db->pager->pages[i]);
        db->pager->pages[i] = NULL;
    }
    pager_close(db->pager);
    free(db);
}
