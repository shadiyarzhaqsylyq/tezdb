CC      = gcc
CFLAGS  = -Wall -Wextra -Iinclude -g -O0
LDFLAGS =

SRCS = src/schema.c src/pager.c src/btree.c src/database.c src/parser.c src/executor.c src/main.c
OBJS = $(SRCS:.c=.o)

TARGET = db

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET) test.db
