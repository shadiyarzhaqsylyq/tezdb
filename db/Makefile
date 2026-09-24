CC = gcc
CFLAGS = -Wall -Wextra -Iinclude
LDFLAGS =

SRCS = src/common.c \
       src/schema.c \
       src/row.c \
       src/expr.c \
       src/pager.c \
       src/btree.c \
       src/parser.c \
       src/executor.c \
       src/shell.c \
       src/main.c

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
