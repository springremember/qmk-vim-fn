# engine/Makefile — host tests only; not part of the QMK build.
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc
SRC      = $(wildcard src/*.c)
TEST_SRC = $(wildcard test/*.c)

# Glue/keymap-common host tests (QMK API stubbed by test/glue/qmk_stub.h).
# The qmk/ sources include "qmk-vim-fn/...", so -I must point at the repo's
# parent directory.
GLUE_ROOT  = $(abspath $(CURDIR)/../..)
GLUE_TESTS = test_glue test_glue_falsify test_nut65_sim test_visual_passthrough test_pending_clear_probe test_strict_clear_falsify test_modifiers test_rgb test_modifier_falsify

.PHONY: all test glue-test clean

all: test

test: $(SRC) $(TEST_SRC)
	$(CC) $(CFLAGS) -o test/kv_tests $(SRC) $(TEST_SRC)
	./test/kv_tests

glue-test: $(SRC) ../qmk/vim_glue.c ../qmk/vim_keymap_common.c
	@for t in $(GLUE_TESTS); do \
		echo "== $$t =="; \
		$(CC) $(CFLAGS) -DQMK_KEYBOARD_H='"qmk_stub.h"' -Itest/glue -I$(GLUE_ROOT) \
			-o test/glue/$$t $(SRC) ../qmk/vim_glue.c ../qmk/vim_keymap_common.c test/glue/$$t.c || exit 1; \
		./test/glue/$$t || exit 1; \
	done

clean:
	rm -f test/kv_tests test/glue/test_glue test/glue/test_glue_falsify test/glue/test_nut65_sim test/glue/test_visual_passthrough test/glue/test_pending_clear_probe test/glue/test_strict_clear_falsify test/glue/test_modifiers test/glue/test_rgb test/glue/test_modifier_falsify
