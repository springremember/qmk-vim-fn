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

# make glue-test 会重写这两个**已跟踪**的二进制（engineering-spec.md §2②）：
# 只有它们允许被 verify-all 的收尾 `git checkout --` 还原。
TRACKED_GLUE_BINS = engine/test/glue/test_adapter_regress engine/test/glue/test_adapter_regress2

.PHONY: all test glue-test matrix-test mutation-test mutation-list verify-all clean

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

# ---------------------------------------------------------------------------
# verify-all：一条命令跑完 engineering-spec.md §2 的全部四道门禁
# ---------------------------------------------------------------------------
# 顺序固定 test → glue-test → matrix-test → mutation-test，**首个失败即停**（其余记 SKIP）；
# 末尾打印 `gate | result | wall time` 汇总表与总墙钟时间。
# 退出码：0 = 全绿；非 0 = 未通过。内部 shell 用 1（有门禁失败）/ 2（收尾自证失败、拒绝报绿）
# 区分两类失败，但 make 对任何失败配方都返回 2，故对外只保证「非 0」；类别看汇总后的
# [verify-all] 行，避免把内部码当稳定接口。
#
# 安全性（必须能在脏工作区上跑）：
#   * 只还原 make glue-test 会改写的那两个**已跟踪**二进制（TRACKED_GLUE_BINS）；
#   * 运行前已存在的其它改动一律不碰，只在汇总里提示；
#   * 收尾后若仍有**跟踪**文件是脏的（相对运行前新增）⇒ 报错退出（退出码 2）；
#   * 新出现的**未跟踪**文件只提示、不判失败 —— 与 test/mutants/run.py 的收尾自证同口径
#     （实测 make matrix-test 期间 vim.tiny 会短暂生成 .swp/.swpx）。
# 可只跑一部分用于自检：make verify-all VERIFY_GATES="test glue-test"
VERIFY_GATES ?= test glue-test matrix-test mutation-test

verify-all:
	@set -u; \
	root=$$(git -C "$(CURDIR)/.." rev-parse --show-toplevel) || { echo "[verify-all] 不在 git 工作区内"; exit 1; }; \
	bins="$(TRACKED_GLUE_BINS)"; \
	base_tracked=$$(git -C "$$root" status --porcelain --untracked-files=no); \
	base_all=$$(git -C "$$root" status --porcelain); \
	pre_bins=$$(git -C "$$root" status --porcelain -- $$bins); \
	if [ -n "$$pre_bins" ]; then \
	  echo "[verify-all] 提示：这两个跟踪二进制在运行前已是脏的，收尾会被还原（仅限这两个文件）："; \
	  printf '%s\n' "$$pre_bins"; \
	fi; \
	rows=""; failed=""; total0=$$(date +%s.%N); \
	for gate in $(VERIFY_GATES); do \
	  if [ -n "$$failed" ]; then rows="$$rows$$gate|SKIP|-\n"; continue; fi; \
	  printf '\n===== make %s =====\n' "$$gate"; \
	  t0=$$(date +%s.%N); \
	  if $(MAKE) --no-print-directory -C "$(CURDIR)" "$$gate"; then rc=0; else rc=$$?; fi; \
	  t1=$$(date +%s.%N); \
	  dt=$$(awk -v a="$$t0" -v b="$$t1" 'BEGIN{printf "%.1f", b-a}'); \
	  if [ "$$rc" -eq 0 ]; then rows="$$rows$$gate|OK|$${dt}s\n"; else rows="$$rows$$gate|FAIL(rc=$$rc)|$${dt}s\n"; failed="$$gate"; fi; \
	  if [ "$$gate" = "glue-test" ]; then \
	    d=$$(git -C "$$root" status --porcelain -- $$bins); \
	    if [ -n "$$d" ]; then git -C "$$root" checkout -- $$bins && echo "[verify-all] 已还原 make glue-test 改写的跟踪二进制"; fi; \
	  fi; \
	done; \
	d=$$(git -C "$$root" status --porcelain -- $$bins); \
	if [ -n "$$d" ]; then git -C "$$root" checkout -- $$bins && echo "[verify-all] 收尾再次还原跟踪二进制"; fi; \
	total1=$$(date +%s.%N); \
	total=$$(awk -v a="$$total0" -v b="$$total1" 'BEGIN{printf "%.1f", b-a}'); \
	after_tracked=$$(git -C "$$root" status --porcelain --untracked-files=no); \
	after_all=$$(git -C "$$root" status --porcelain); \
	bt=$$(mktemp); at=$$(mktemp); ba=$$(mktemp); aa=$$(mktemp); \
	printf '%s\n' "$$base_tracked" | sed '/^$$/d' | sort > "$$bt"; \
	printf '%s\n' "$$after_tracked" | sed '/^$$/d' | sort > "$$at"; \
	printf '%s\n' "$$base_all" | sed '/^$$/d' | sort > "$$ba"; \
	printf '%s\n' "$$after_all" | sed '/^$$/d' | sort > "$$aa"; \
	new_tracked=$$(comm -13 "$$bt" "$$at"); \
	pre_tracked=$$(comm -12 "$$bt" "$$at"); \
	new_untracked=$$(comm -13 "$$ba" "$$aa" | grep '^??' || true); \
	rm -f "$$bt" "$$at" "$$ba" "$$aa"; \
	printf '\n========== verify-all 汇总 ==========\n'; \
	printf '%-14s | %-11s | %s\n' "gate" "result" "wall time"; \
	printf '%-14s-+-%-11s-+-%s\n' "--------------" "-----------" "---------"; \
	printf '%b' "$$rows" | while IFS='|' read -r g r w; do printf '%-14s | %-11s | %s\n' "$$g" "$$r" "$$w"; done; \
	printf '总墙钟时间：%s s\n' "$$total"; \
	if [ -n "$$pre_tracked" ]; then \
	  printf '[verify-all] 提示：运行前工作区已有跟踪文件改动（未触碰、未还原）：\n%s\n' "$$pre_tracked"; \
	fi; \
	if [ -n "$$new_untracked" ]; then \
	  printf '[verify-all] 提示：出现新的未跟踪文件（只提示，不判失败）：%s\n' "$$(printf '%s' "$$new_untracked" | tr '\n' ' ')"; \
	fi; \
	a=0; \
	if [ -n "$$new_tracked" ]; then \
	  printf '[verify-all] 失败：收尾后仍有**跟踪**文件是脏的，拒绝报绿：\n%s\n' "$$new_tracked"; a=2; \
	fi; \
	if [ -n "$$d" ]; then \
	  printf '[verify-all] 失败：这两个跟踪二进制在还原后仍不干净（含暂存区），拒绝报绿：\n%s\n' "$$d"; a=2; \
	fi; \
	if [ -n "$$failed" ]; then \
	  printf '[verify-all] 失败：门禁 %s 未通过，其后的门禁已跳过。\n' "$$failed"; [ "$$a" -ne 0 ] || a=1; \
	fi; \
	if [ "$$a" -eq 0 ]; then \
	  printf '[verify-all] 全绿：%s 道门禁全部 OK。\n' "$$(printf '%b' "$$rows" | grep -c '|OK|')"; \
	fi; \
	exit $$a

clean:
	rm -f test/kv_tests test/host/probe test/glue/test_adapter_regress test/glue/test_adapter_regress2 test/glue/test_glue test/glue/test_glue_falsify test/glue/test_visual_passthrough test/glue/test_pending_clear_probe test/glue/test_strict_clear_falsify test/glue/test_modifiers test/glue/test_rgb test/glue/test_modifier_falsify
