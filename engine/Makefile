# engine/Makefile — host tests only; not part of the QMK build.
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc
SRC      = $(wildcard src/*.c)
TEST_SRC = $(wildcard test/*.c)

# Glue/keymap-common host tests (QMK API stubbed by test/glue/qmk_stub.h).
# The qmk/ sources include "qmk-vim-fn/...", so -I must point at the repo's
# parent directory.
GLUE_ROOT  = $(abspath $(CURDIR)/../..)
GLUE_TESTS = test_glue test_glue_falsify test_visual_passthrough test_pending_clear_probe test_strict_clear_falsify test_modifiers test_rgb test_modifier_falsify test_adapter_regress test_adapter_regress2

.PHONY: all test glue-test matrix-test mutation-test mutation-list clean

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

# Repo-resident matrix test: compare the engine's host-visible key stream
# against real Vim (vim.tiny) through the host-editor model in
# test/host/kvhost.py.  Exit status is 0 only when every case matches Vim or
# is a declared deviation (vim/design.md §4.9 ①–⑩, see test/host/matrix.py).
matrix-test: $(SRC)
	$(CC) $(CFLAGS) -o test/host/probe test/host/probe.c $(SRC)
	python3 test/host/matrix.py

# 变异验证门禁（qmk/engineering-spec.md §1.1）：把 test/mutants/mutants.txt 里的每条
# 记录暂时施加到源码上、跑对应套件、分类 CAUGHT/SURVIVED，然后**无条件还原**。
# 默认跑全部；退出码 0 = 全绿（含已论证的等价变异），1 = 有未论证的 SURVIVED，
# 2 = 门禁自身错误（记录漂移 / 工作区脏 / 超时）。不交互、确定性顺序。
mutation-test:
	python3 test/mutants/run.py

mutation-list:
	python3 test/mutants/run.py --list

clean:
	rm -f test/kv_tests test/host/probe test/glue/test_adapter_regress test/glue/test_adapter_regress2 test/glue/test_glue test/glue/test_glue_falsify test/glue/test_visual_passthrough test/glue/test_pending_clear_probe test/glue/test_strict_clear_falsify test/glue/test_modifiers test/glue/test_rgb test/glue/test_modifier_falsify
