# engine/Makefile — host tests only; not part of the QMK build.
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc
SRC      = $(wildcard src/*.c)
TEST_SRC = $(wildcard test/*.c)

.PHONY: all test clean

all: test

test: $(SRC) $(TEST_SRC)
	$(CC) $(CFLAGS) -o test/kv_tests $(SRC) $(TEST_SRC)
	./test/kv_tests

clean:
	rm -f test/kv_tests
