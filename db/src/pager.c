#include "pager.h"

Pager* pager_open(const char* filename) {
    int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    if (fd == -1) {
        printf("Unable to open file\n");
        exit(EXIT_FAILURE);
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        printf("Error obtaining file stats: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    Pager* pager = (Pager*)malloc(sizeof(Pager));
    pager->file_descriptor = fd;
    pager->file_length = (uint32_t)st.st_size;
    pager->num_pages = pager->file_length / PAGE_SIZE;

    if (pager->file_length % PAGE_SIZE != 0) {
        printf("Db file is not a whole number of pages. Corrupt file.\n");
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->pages[i] = NULL;
    }

    return pager;
}

void* pager_get_page(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) {
        printf("Tried to fetch page number out of bounds. %u >= %d\n",
               page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    if (pager->pages[page_num] == NULL) {
        void* page = malloc(PAGE_SIZE);
        uint32_t npages = pager->file_length / PAGE_SIZE;
        if (pager->file_length % PAGE_SIZE) {
            npages += 1;
        }
        if (page_num <= npages) {
            off_t offset = (off_t)page_num * PAGE_SIZE;
            ssize_t bytes_read = pread(pager->file_descriptor, page, PAGE_SIZE, offset);
            if (bytes_read == -1) {
                printf("Error reading file: %d\n", errno);
                exit(EXIT_FAILURE);
            }
        }
        pager->pages[page_num] = page;
        if (page_num >= pager->num_pages) {
            pager->num_pages = page_num + 1;
        }
    }
    return pager->pages[page_num];
}

void pager_flush(Pager* pager, uint32_t page_num) {
    if (pager->pages[page_num] == NULL) {
        printf("Tried to flush null page\n");
        exit(EXIT_FAILURE);
    }

    off_t offset = (off_t)page_num * PAGE_SIZE;
    ssize_t bytes_written = pwrite(pager->file_descriptor,
                                   pager->pages[page_num], PAGE_SIZE, offset);
    if (bytes_written == -1) {
        printf("Error writing: %d\n", errno);
        exit(EXIT_FAILURE);
    }
}

void pager_close(Pager* pager) {
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (pager->pages[i] != NULL) {
            free(pager->pages[i]);
            pager->pages[i] = NULL;
        }
    }
    if (pager->file_descriptor != -1) {
        close(pager->file_descriptor);
    }
    free(pager);
}
