#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#define PAGE_SIZE 4096
#define MAX_ROWS 50


void db_open() {
    int fd = open("database.bin", O_WRONLY | O_CREAT, 0644);
    
    // 1. Your text data
    const char *text = "Hello, World";
    
    // 2. Point to the bytes and calculate the byte size
    const void *buf = (const void *)text;
    size_t byte_count = strlen(text); // 12 bytes
    off_t offset = 0;

    // 3. Write directly to disk
    ssize_t bytes_written = pwrite(fd, buf, byte_count, offset); // or pwrite(fd, text, byte_count, offset);
    
    if (bytes_written == -1) {
        perror("Write failed");
    }

    close(fd);
   
}


int main() {
/*
	char input_buffer[1024];
    if (argc <= 1) {
        printf("Usage: %s <db_filename>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
	
	bool dbopen = false;
    const char* filename = argv[1];
    //open(filename);
	

        while (dbopen) {
            printf("db=# ");
            fflush(stdout);
       }
	fgets();
    return 0;
*/
db_open();

}
