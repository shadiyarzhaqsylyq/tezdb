CC      = gcc
CFLAGS  = -Wall -Wextra -Iinclude -g -O0
LDFLAGS =

OBJS = \
	src/schema.o \
	src/pager.o \
	src/btree.o \
	src/database.o \
	src/parser.o \
	src/executor.o \
	src/main.o

.PHONY: all clean

all: db

db: $(OBJS)
	$(CC) $(CFLAGS) -o db $(OBJS) $(LDFLAGS)

src/schema.o: src/schema.c include/schema.h include/common.h
	$(CC) $(CFLAGS) -c -o src/schema.o src/schema.c

src/pager.o: src/pager.c include/pager.h include/common.h
	$(CC) $(CFLAGS) -c -o src/pager.o src/pager.c

src/btree.o: src/btree.c include/btree.h include/database.h include/common.h
	$(CC) $(CFLAGS) -c -o src/btree.o src/btree.c

src/database.o: src/database.c include/database.h include/common.h
	$(CC) $(CFLAGS) -c -o src/database.o src/database.c

src/parser.o: src/parser.c include/parser.h include/common.h
	$(CC) $(CFLAGS) -c -o src/parser.o src/parser.c

src/executor.o: src/executor.c include/executor.h include/common.h
	$(CC) $(CFLAGS) -c -o src/executor.o src/executor.c

src/main.o: src/main.c include/common.h
	$(CC) $(CFLAGS) -c -o src/main.o src/main.c

clean:
	rm -f $(OBJS) db
