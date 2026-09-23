#ifndef DB_PAGER_H
#define DB_PAGER_H

#include "common.h"

struct Pager {
    int file_descriptor;
    uint32_t file_length;
    uint32_t num_pages;
    void* pages[TABLE_MAX_PAGES];
};

Pager* pager_open(const char* filename);
void*  pager_get_page(Pager* pager, uint32_t page_num);
void   pager_flush(Pager* pager, uint32_t page_num);
void   pager_close(Pager* pager);

#endif /* DB_PAGER_H */
