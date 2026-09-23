CC      = gcc
CFLAGS  = -Wall -Wextra -Iinclude -g -O0
LDFLAGS =

OBJS = \
	main/schema.o \
	main/pager.o \
	main/btree.o \
	main/database.o \
	main/parser.o \
	main/executor.o \
	main/main.o

.PHONY: all clean

all: db

db: $(OBJS)
	$(CC) $(CFLAGS) -o db $(OBJS) $(LDFLAGS)

src/schema.o: src/schema.c include/schema.h include/common.h
	$(CC) $(CFLAGS) -c -o main/schema.o main/schema.c

src/pager.o: src/pager.c include/pager.h include/common.h
	$(CC) $(CFLAGS) -c -o main/pager.o main/pager.c

src/btree.o: src/btree.c include/btree.h include/database.h include/common.h
	$(CC) $(CFLAGS) -c -o main/btree.o main/btree.c

src/database.o: src/database.c include/database.h include/common.h
	$(CC) $(CFLAGS) -c -o main/database.o main/database.c

src/parser.o: src/parser.c include/parser.h include/common.h
	$(CC) $(CFLAGS) -c -o main/parser.o main/parser.c

src/executor.o: src/executor.c include/executor.h include/common.h
	$(CC) $(CFLAGS) -c -o main/executor.o main/executor.c

src/main.o: src/main.c include/common.h
	$(CC) $(CFLAGS) -c -o main/main.o main/main.c

clean:
	rm -f $(OBJS) db
