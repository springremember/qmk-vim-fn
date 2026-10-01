#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""引擎变异门禁（mutation gate）—— qmk/engineering-spec.md §1.1 的自动化。

把 mutants.txt 里的每条记录当作一次「把实现改坏」：
施加整行文本替换 → 跑该记录声明的套件 → 分类 → **无条件还原**。

  CAUGHT      套件失败（断言红或编译失败）⇒ 该行为被测试钉住
  SURVIVED    套件全绿 ⇒ 失败（除非记录标了 equiv = yes）
  EQUIVALENT  标了 equiv = yes 的记录 SURVIVED ⇒ 允许，另行列出
  ERROR       记录漂移 / 超时 / 工作区脏等门禁自身故障（退出码 2）

退出码：0 = 全绿（含允许的等价变异）；1 = 有未论证的 SURVIVED；2 = 门禁自身错误；130 = SIGINT。
只用标准库；无交互提示；记录顺序 = 执行顺序（确定性）。

用法（在 engine/ 下）：
    make mutation-test                  # = python3 test/mutants/run.py
    python3 test/mutants/run.py --list
    python3 test/mutants/run.py --only d24-drop-up-flip,budget-room-unbounded
    python3 test/mutants/run.py --suite glue
    python3 test/mutants/run.py --quiet
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]                    # <repo>/engine/test/mutants -> <repo>
ENGINE_DIR = REPO_ROOT / "engine"
DEFAULT_RECORDS = HERE / "mutants.txt"

# make glue-test 会重写这两个**已跟踪**的二进制（engineering-spec.md §2②）：
# 每次跑完必须 git checkout -- 还原，绝不留下脏工作区。
TRACKED_GLUE_BINS = (
    "engine/test/glue/test_adapter_regress",
    "engine/test/glue/test_adapter_regress2",
)

SUITES = {
    "engine": ["make", "test"],        # ./test/kv_tests，退出码非 0 即红
    "glue": ["make", "glue-test"],     # 10 个独立套件，首个失败即停
}
EXIT_OK, EXIT_SURVIVED, EXIT_ERROR, EXIT_INTERRUPT = 0, 1, 2, 130

# 当前被改写的文件（路径, 原始字节）；信号处理与 finally 都靠它保证还原。
_current: "tuple[Path, bytes] | None" = None
_touched: "dict[str, bytes]" = {}     # 相对路径 -> 运行前字节（用于收尾自证）
_child: "subprocess.Popen | None" = None   # 当前套件进程（独立进程组，便于整组终止）


# --------------------------------------------------------------------------
# 记录解析
# --------------------------------------------------------------------------
class RecordError(Exception):
    pass


class Mutant:
    __slots__ = ("id", "file", "suite", "equiv", "why", "old", "new", "line", "seen_new")

    def __init__(self, mid: str, line: int):
        self.id = mid
        self.file = ""
        self.suite = ""
        self.equiv = False
        self.why = ""
        self.old = ""
        self.new = ""
        self.line = line
        self.seen_new = False

    @property
    def path(self) -> Path:
        return REPO_ROOT / self.file

    def __repr__(self) -> str:  # pragma: no cover - debug aid
        return "Mutant(%s)" % self.id


def parse_records(path: Path) -> "list[Mutant]":
    """解析 mutants.txt（格式见该文件头部注释）。"""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RecordError("无法读取记录表 %s: %s" % (path, exc))

    records: "list[Mutant]" = []
    cur: "Mutant | None" = None
    mode: "str | None" = None            # None | "old" | "new"
    why_seen = False

    for lineno, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip("\r")
        stripped = line.strip()

        if mode is not None:             # ---- old/new 块内部：逐字照收 ----
            if stripped == "new" and mode == "old":
                mode = "new"
                cur.seen_new = True
                continue
            if stripped == "end":
                if not cur.seen_new:
                    raise RecordError("%s:%d: 记录缺少 `new` 段（整段删除也要写空的 "
                                      "`new`…`end`）" % (path, lineno))
                records.append(cur)
                cur, mode = None, None
                continue
            if stripped.startswith("mutant "):
                raise RecordError("%s:%d: 上一条记录缺少 `end`" % (path, lineno))
            if mode == "old":
                cur.old += line + "\n"
            else:
                cur.new += line + "\n"
            continue

        if not stripped or stripped.startswith("#"):
            continue
        if stripped.startswith("mutant "):
            if cur is not None:
                raise RecordError("%s:%d: 上一条记录缺少 `end`" % (path, lineno))
            mid = stripped[len("mutant "):].strip()
            if not mid:
                raise RecordError("%s:%d: mutant 缺少 id" % (path, lineno))
            cur = Mutant(mid, lineno)
            why_seen = False
            continue
        if stripped in ("old", "new"):
            if cur is None:
                raise RecordError("%s:%d: `%s` 出现在记录之外" % (path, lineno, stripped))
            if stripped == "new":
                raise RecordError("%s:%d: `new` 出现在 `old` 之前" % (path, lineno))
            mode = "old"
            continue
        if stripped == "end":
            raise RecordError("%s:%d: 多余的 `end`" % (path, lineno))
        if "=" in stripped:
            if cur is None:
                raise RecordError("%s:%d: 字段出现在记录之外" % (path, lineno))
            key, val = stripped.split("=", 1)
            key, val = key.strip(), val.strip()
            if key == "file":
                cur.file = val
            elif key == "suite":
                cur.suite = val
            elif key == "equiv":
                if val not in ("yes", "no", "true", "false"):
                    raise RecordError("%s:%d: equiv 只能是 yes/no" % (path, lineno))
                cur.equiv = val in ("yes", "true")
            elif key == "why":
                cur.why = val
                why_seen = True
            else:
                raise RecordError("%s:%d: 未知字段 `%s`" % (path, lineno, key))
            continue
        raise RecordError("%s:%d: 无法解析的行 `%s`" % (path, lineno, line))

    if mode is not None or cur is not None:
        raise RecordError("%s: 文件结束前有未闭合的记录" % path)

    # 逐条自检
    seen: "set[str]" = set()
    for m in records:
        where = "%s:%d (%s)" % (path, m.line, m.id)
        if m.id in seen:
            raise RecordError("%s: id 重复" % where)
        seen.add(m.id)
        if not m.file:
            raise RecordError("%s: 缺少 file" % where)
        if not (REPO_ROOT / m.file).is_file():
            raise RecordError("%s: file 不存在 %s" % (where, m.file))
        if m.file.startswith("/") or ".." in Path(m.file).parts:
            raise RecordError("%s: file 必须是仓库内相对路径" % where)
        if m.suite not in SUITES:
            raise RecordError("%s: suite 必须是 %s 之一" % (where, "/".join(SUITES)))
        if not m.why:
            raise RecordError("%s: 缺少 why（说明改坏什么行为 / 等价性论证）" % where)
        if m.equiv and not why_seen:
            raise RecordError("%s: equiv=yes 必须带 why 论证" % where)
        if m.old == "":
            raise RecordError("%s: old 为空" % where)
        if m.old == m.new:
            raise RecordError("%s: old 与 new 相同" % where)
    return records


# --------------------------------------------------------------------------
# git / 套件辅助
# --------------------------------------------------------------------------
def git(*args: str) -> "subprocess.CompletedProcess[str]":
    return subprocess.run(
        ["git", "-C", str(REPO_ROOT), *args],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
    )


def porcelain() -> "list[str]":
    out = git("status", "--porcelain").stdout
    return [l for l in out.splitlines() if l.strip()]


def restore_tracked_bins(reason: str = "") -> None:
    """还原 make glue-test 覆盖的两个已跟踪二进制。"""
    dirty = [p for p in TRACKED_GLUE_BINS if git("status", "--porcelain", "--", p).stdout.strip()]
    if not dirty:
        return
    res = git("checkout", "--", *dirty)
    if res.returncode != 0:
        sys.stderr.write("[mutants] 还原 %s 失败%s: %s\n"
                         % (", ".join(dirty), ("（" + reason + "）") if reason else "",
                            res.stdout.strip()))
    else:
        sys.stderr.write("[mutants] 已还原被 make glue-test 改写的跟踪二进制：%s\n"
                         % ", ".join(dirty))


def _restore_current() -> None:
    global _current
    if _current is None:
        return
    path, data = _current
    try:
        with open(path, "wb") as fh:
            fh.write(data)
    except OSError as exc:                        # 绝不静默
        sys.stderr.write("[mutants] 致命：还原 %s 失败: %s\n" % (path, exc))
    _current = None


def _verify_touched() -> "list[str]":
    """收尾自证：(1) 每个被碰过的文件与运行前逐字节相同；(2) 额外脏文件为 0。"""
    problems = []
    for rel, want in _touched.items():
        p = REPO_ROOT / rel
        try:
            got = p.read_bytes()
        except OSError as exc:
            problems.append("%s 不可读: %s" % (rel, exc))
            continue
        if got != want:
            problems.append("%s 与运行前不一致（%d != %d 字节）" % (rel, len(got), len(want)))
    return problems


def _kill_child() -> None:
    """终止当前套件（含其 cc 子进程）：整组 SIGTERM，避免它在我们还原之后才写完文件。"""
    global _child
    proc, _child = _child, None
    if proc is None or proc.poll() is not None:
        return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except OSError:
        try:
            proc.terminate()
        except OSError:
            pass
    try:
        proc.wait(timeout=10)
    except Exception:                              # noqa: BLE001 - 兜底强杀
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except OSError:
            pass


def _on_signal(signum, _frame):                   # noqa: ANN001 - signal handler
    _kill_child()
    _restore_current()
    restore_tracked_bins("SIGINT/SIGTERM")
    sys.stderr.write("\n[mutants] 收到信号 %d：已终止套件并还原被改文件，中断（退出码 %d）\n"
                     % (signum, EXIT_INTERRUPT))
    sys.stderr.flush()
    os._exit(EXIT_INTERRUPT)


# --------------------------------------------------------------------------
# 施加 / 运行 / 分类
# --------------------------------------------------------------------------
def apply_mutant(m: Mutant) -> None:
    global _current
    path = m.path
    if m.file not in _touched:
        _touched[m.file] = path.read_bytes()
    data = path.read_bytes()
    old_b, new_b = m.old.encode("utf-8"), m.new.encode("utf-8")
    if data.count(old_b) != 1:
        raise RecordError("%s: old 文本在 %s 中匹配 %d 次（要求恰好 1 次）—— 代码已漂移，"
                          "请更新该记录" % (m.id, m.file, data.count(old_b)))
    path.write_bytes(data.replace(old_b, new_b, 1))
    _current = (path, data)


def run_suite(name: str, timeout: float) -> "tuple[int, str, float, bool]":
    global _child
    cmd = SUITES[name]
    t0 = time.monotonic()
    try:
        proc = subprocess.Popen(cmd, cwd=str(ENGINE_DIR), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace",
                                start_new_session=True)
    except OSError as exc:
        raise RecordError("无法执行 %s: %s" % (" ".join(cmd), exc))
    _child = proc
    timed_out = False
    try:
        out, _ = proc.communicate(timeout=timeout)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        timed_out = True
        _kill_child()
        out, _ = proc.communicate()
        out = (out or "") + "\n[timeout after %.0fs]\n" % timeout
        rc = -1
    finally:
        _child = None
    return rc, out or "", time.monotonic() - t0, timed_out


def failure_excerpt(out: str) -> str:
    """从套件输出里挑出"为什么红"的证据：优先 fail=N(N>0)/error:/FAIL 行。"""
    red, any_fail, tail = [], [], []
    for line in out.splitlines():
        s = line.strip()
        if not s:
            continue
        tail.append(s)
        if re.search(r"fail=[1-9]\d*", line) or "error:" in line \
           or s.startswith("FAIL") or " FAIL " in line or "错误" in line:
            red.append(s)
        elif "fail=" in line:
            any_fail.append(s)
    hits = red or any_fail or tail
    return " | ".join(hits[:4] if red else hits[-3:])


def _print_table(rows: "list[tuple[str, str, str, str]]", out=sys.stdout) -> None:
    head = ("id", "file", "suite", "result")
    w = [len(h) for h in head]
    for r in rows:
        for i in range(4):
            w[i] = max(w[i], len(r[i]))
    out.write("  ".join(h.ljust(w[i]) for i, h in enumerate(head)) + "\n")
    out.write("  ".join("-" * w[i] for i in range(4)) + "\n")
    for r in rows:
        out.write("  ".join(r[i].ljust(w[i]) for i in range(4)) + "\n")


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(
        prog="run.py",
        description="声明式变异门禁：施加 mutants.txt 的替换、跑套件、分类、还原。",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--records", type=Path, default=DEFAULT_RECORDS,
                    help="记录表路径（默认 %s）" % DEFAULT_RECORDS.name)
    ap.add_argument("--list", action="store_true", help="只列出变异（不跑套件）")
    ap.add_argument("--only", metavar="ID[,ID...]", help="只跑指定 id（逗号分隔）")
    ap.add_argument("--suite", choices=sorted(SUITES), help="只跑指定套件的记录")
    ap.add_argument("--quiet", action="store_true", help="静默：不打印逐条进度与套件输出")
    ap.add_argument("--timeout", type=float, default=300.0, metavar="SEC",
                    help="单条套件超时秒数（默认 300）")
    args = ap.parse_args(argv)

    quiet = args.quiet
    try:
        records = parse_records(args.records)
    except RecordError as exc:
        sys.stderr.write("[mutants] 记录表错误：%s\n" % exc)
        return EXIT_ERROR

    if args.only:
        want = [x.strip() for x in args.only.split(",") if x.strip()]
        known = {m.id for m in records}
        unknown = [x for x in want if x not in known]
        if unknown:
            sys.stderr.write("[mutants] --only 指定了未知 id：%s\n" % ", ".join(unknown))
            return EXIT_ERROR
        order = {x: i for i, x in enumerate(want)}
        records = sorted([m for m in records if m.id in order], key=lambda m: order[m.id])
    if args.suite:
        records = [m for m in records if m.suite == args.suite]
    if not records:
        sys.stderr.write("[mutants] 没有匹配的记录\n")
        return EXIT_ERROR

    if args.list:
        rows = [(m.id, m.file, m.suite, "EQUIVALENT(声明)" if m.equiv else "expect CAUGHT")
                for m in records]
        _print_table(rows)
        print("\n共 %d 条；why 见记录表（equiv=yes 的记录允许 SURVIVED）" % len(records))
        return EXIT_OK

    # ---- 前置：漂移自检 + 目标文件必须干净 ----
    drift = []
    for m in records:
        n = m.path.read_bytes().count(m.old.encode("utf-8"))
        if n != 1:
            drift.append("%s: old 在 %s 中匹配 %d 次（要求恰好 1 次）" % (m.id, m.file, n))
    if drift:
        sys.stderr.write("[mutants] 记录漂移，未运行任何变异：\n")
        for d in drift:
            sys.stderr.write("  - %s\n" % d)
        return EXIT_ERROR

    targets = sorted({m.file for m in records} | set(TRACKED_GLUE_BINS))
    dirty = porcelain()
    dirty_targets = [l for l in dirty if any(l.endswith(t) or t in l for t in targets)]
    if dirty_targets:
        sys.stderr.write("[mutants] 目标文件在工作区里已是脏的，拒绝运行（先 commit/stash）：\n")
        for l in dirty_targets:
            sys.stderr.write("  %s\n" % l)
        return EXIT_ERROR
    baseline = set(dirty)

    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    results = []            # (id, file, suite, result)
    survivors, equivalents, errors, caught = [], [], [], []
    try:
        for i, m in enumerate(records, 1):
            label = "[%2d/%2d] %-34s %-6s" % (i, len(records), m.id, m.suite)
            if not quiet:
                sys.stdout.write(label + " ... ")
                sys.stdout.flush()
            try:
                apply_mutant(m)
            except RecordError as exc:
                errors.append((m, str(exc)))
                results.append((m.id, m.file, m.suite, "ERROR"))
                if not quiet:
                    sys.stdout.write("ERROR\n")
                sys.stderr.write("[mutants] %s\n" % exc)
                continue
            try:
                rc, out, dt, timed_out = run_suite(m.suite, args.timeout)
            finally:
                _restore_current()
                if m.suite == "glue":
                    restore_tracked_bins(m.id)

            if timed_out:
                errors.append((m, "套件超时 %.0fs（可能被变异改成死循环）" % args.timeout))
                result = "ERROR"
            elif rc != 0:
                result = "CAUGHT"
                caught.append(m)
            elif m.equiv:
                result = "EQUIVALENT"
                equivalents.append(m)
            else:
                result = "SURVIVED"
                survivors.append(m)
            results.append((m.id, m.file, m.suite, result))

            if not quiet:
                sys.stdout.write("%s  (%.1fs, rc=%d)\n" % (result, dt, rc))
                if result == "CAUGHT":
                    print("        ↳ %s" % failure_excerpt(out))
                elif result in ("SURVIVED", "EQUIVALENT"):
                    greens = [l.strip() for l in out.splitlines() if "pass=" in l]
                    print("        ↳ 套件全绿：%s" % (greens[-1] if greens else "no output"))
            elif result in ("SURVIVED",):
                print("SURVIVED: %s (%s, %s)" % (m.id, m.file, m.suite))
    except KeyboardInterrupt:                       # 信号处理器兜底之外的路径
        _kill_child()
        _restore_current()
        restore_tracked_bins("KeyboardInterrupt")
        sys.stderr.write("\n[mutants] 中断：已还原被改文件\n")
        return EXIT_INTERRUPT
    finally:
        _restore_current()
        restore_tracked_bins("收尾")

    # ---- 收尾自证：工作区必须干净 ----
    clean_problems = _verify_touched()
    now = set(porcelain())
    extra = sorted(now - baseline)
    if extra:
        clean_problems.append("出现未预期的脏文件：%s" % ", ".join(extra))

    print()
    _print_table(results)
    print("\nCAUGHT %d  EQUIVALENT %d  SURVIVED %d  ERROR %d  （共 %d 条）"
          % (len(caught), len(equivalents), len(survivors), len(errors), len(results)))
    for m in equivalents:
        print("  EQUIVALENT %s —— %s" % (m.id, m.why))
    for m in survivors:
        print("  SURVIVED   %s —— 测试没钉住：%s" % (m.id, m.why))
    for m, msg in errors:
        print("  ERROR      %s —— %s" % (m.id, msg))

    if clean_problems:
        sys.stderr.write("\n[mutants] 工作区自证失败，拒绝报绿：\n")
        for p in clean_problems:
            sys.stderr.write("  - %s\n" % p)
        return EXIT_ERROR
    if not quiet:
        st = git("status", "--porcelain", "--", *targets).stdout.strip()
        sys.stderr.write("[mutants] 收尾自证：%d 个被碰文件与运行前逐字节一致；"
                         "目标文件 git status %s\n"
                         % (len(_touched), "干净" if not st else "脏：" + st))
    if errors:
        return EXIT_ERROR
    if survivors:
        return EXIT_SURVIVED
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
