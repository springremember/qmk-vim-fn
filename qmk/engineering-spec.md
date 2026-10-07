# 工程规范与验收基线

> 本文件是**工程流程与验收标准的唯一权威**（「什么算绿、怎么证明绿」）。与
> [`README.md`](README.md)（共享层 ↔ 键盘分支的同步与发布规范）同级：
> 本文管**做法与判据**，`qmk/README.md` 管**同步/归档操作**。
> 设计权威仍是 [`../vim/design.md`](../vim/design.md)（引擎）与
> [`../caps/design.md`](../caps/design.md)（Caps 模块）。
>
> 适用范围：`qmk-vim-fn` 共享层（`engine/` + `qmk/`）的一切修改。
> 键盘仓库（`qmk_firmware` 的 `qk61` / `nut65`）的同步与归档见 [`README.md`](README.md) §3–§6。
>
> **数字纪律**：本文件里所有测试/体积数字都必须**亲自实测**并在改动时更新；禁止把估算、
> 记忆或他人转述的数字写成实测。每条基线都注明命令与测量时点（HEAD）。

---

## 1. 工程流程（硬性顺序）

一次修复/变更必须按下列顺序推进，**缺一不可**：

1. **文档先行**：先提交规范/设计（`vim/{design,readme,changes,testcase}.md`、
   `caps/{design,readme,changes,testcase}.md`、`fn/{readme,changes}.md`、`qmk/README.md`、
   **本文件**）。
2. **文档单独提交**：该提交**只含文档**且**早于**任何源码/测试提交。同一提交里既改文档又改代码
   = **回填，违规**。
3. **测试先红**：按已提交的文档写用例（可单独提交）。此阶段测试**必须失败**（编译失败或断言红），
   这本身就是"测试先行"的证据；把红灯输出记进 `vim/changes.md`。
4. **才允许改实现**：`engine/` + `qmk/` 对齐已提交的文档；**实现提交里不得夹带文档改动**
   （含头文件里的契约注释）。
5. **验证与登记**：跑完 §2 的三条基线 + §1.1 的变异验证 + §1.4 的独立复核，把缺陷编号与出处
   写入 `vim/changes.md`，再按 `qmk/README.md` §3–§6 同步/归档。

> 违规的历史代价：共享层与子模块漂移出「内容相同、哈希不同」的两份共享层；文档承诺 `cfg` 字段
> 而代码只写不读（`qmk/README.md` §6 坑位 1/7）。

### 1.1 变异验证（每条修复都要证明"测试是有效的"）

- **定义**：把刚修好的实现**改坏**（回退成旧实现 / 去掉关键键码 / 翻转判据），重跑测试：
  - **CAUGHT** = 至少一条断言变红 ⇒ 该修复**有测试钉住**；
  - **SURVIVED** = 全绿 ⇒ **不得当作通过**：必须论证它是**等价变异**（改法语义恒等，例如某分支
    只会以 `n=1` 到达、截断恒等），或**补齐覆盖**后重测。
- 每条变异都要在 `changes.md` 里记「改坏什么 → pass/fail 数 → CAUGHT/SURVIVED + 理由」。
  已登记的等价变异示例：`vim/changes.md` §7.33（31 处变异，29 CAUGHT，2 处论证等价）。
- **容量型/结构性缺陷**（如键码撑爆队列）逻辑变异可能覆盖不到，必须另做**前后探针对照**（§1.5）。

#### 1.1.1 自动化门禁 `make mutation-test`

上面的手工流程（改坏 → 跑套件 → 还原 → 记数）已固化为**一条命令**，不再手工注入：

```sh
cd engine
make mutation-test              # = python3 test/mutants/run.py（仅标准库、无交互）
make mutation-list              # 只列记录
python3 test/mutants/run.py --only d24-drop-up-flip,budget-room-unbounded
python3 test/mutants/run.py --suite glue      # 只跑 glue 记录
python3 test/mutants/run.py --quiet           # 只留汇总
```

- **记录表** = `engine/test/mutants/mutants.txt`（人类可编辑、diff 友好）。每条记录是一次
  **整行文本替换**：

  ```
  mutant <id>                     # 稳定、全局唯一的 id
  file = engine/src/command.c     # 仓库相对路径
  suite = engine | glue           # engine = make test；glue = make glue-test
  equiv = no | yes                # yes = 已论证的等价变异（SURVIVED 不判失败）
  why = <改坏什么行为；equiv=yes 时写等价性论证>
  old
  <逐行原文：必须与当前源码逐字节一致且唯一匹配，否则门禁 drift 报错、退出码 2>
  new
  <替换文本（可为空 = 整段删除）>
  end
  ```

  选自定义块格式而不是 JSON：`old`/`new` 是整段 C 代码，转义成 `\n` 会同时毁掉
  **可读性**与**评审价值**（评审者要能直接看到补丁本身）。块内每行自带结尾换行，
  所以补丁永远是整行替换，不会吃掉/留下半行。
- **分类**（每条独立施加、独立还原）：
  - **CAUGHT**：套件非 0 退出（断言红**或编译失败**）⇒ 该行为被测试钉住；
  - **SURVIVED**：套件全绿且未标 `equiv` ⇒ **门禁退出码 1**（要么改法无意义、要么测试太弱）；
  - **EQUIVALENT**：标了 `equiv = yes` 的记录 SURVIVED ⇒ 汇总里单列，**不**判失败；
    若标了 `equiv` 却被 CAUGHT，照常记 CAUGHT（说明等价性声明已过期，需重审 `why`）。
- **还原是硬保证**：替换前把原件另存到 `mkdtemp` **临时备份目录**（路径在启动时打印、正常结束
  删除；即使进程被 `SIGKILL` 也丢不了原件）并读进内存；`finally`、SIGINT/SIGTERM 处理器都还原，
  并会终止正在跑的套件进程组（避免 `make` 在我们还原之后才写完文件）。收尾自证三条**硬判据**：
  (1) 每个被碰文件与运行前**逐字节一致**；(2) 目标文件（被变异文件 + `make glue-test` 覆盖的
  两个已跟踪二进制，§2②）`git status` 干净；(3) 全仓**跟踪**状态相对运行前无新增改动 ——
  任一不满足即**拒绝报绿**（退出码 2）。新出现的**未跟踪**文件只提示、不判死：实测
  `make matrix-test` 期间 `vim.tiny` 会短暂生成 `engine/.swp`/`.swpx`，那不是变异没还原。
  两个已跟踪二进制由 runner 自己 `git checkout --` 还原，绝不留给人工。
- **CI 语义**：无提示、无交互；执行顺序 = 记录顺序（确定性）；退出码
  `0` 全绿 / `1` 有 SURVIVED / `2` 门禁自身错误（漂移、脏工作区、无法解析、超时）/
  `130` 被 SIGINT 中断；`--timeout SEC` 防止变异把套件改成死循环时挂住 CI。
- **加一条变异**：在 `mutants.txt` 末尾复制一组 `mutant … end`，把 `old` 从当前源码
  **逐字**粘进去即可 —— 一行代码都不用改（见本节末的实测清单）。
- **实测（2026-10-01，HEAD `927ecb1` + 本提交）**：16 条记录 ⇒
  **CAUGHT 15 / EQUIVALENT 1 / SURVIVED 0**，整轮 **≈50 s**（engine 13 条 × ~1.7 s，
  glue 3 条 × ~9 s；`make matrix-test` 的 ~2 min 不参与）。覆盖：键码预算
  （`kv_emit_room`/`kv_emit_clamp_n`）、D13/D19/D20/D22/D23/D24 的关键键码与落点、
  环形下标、Caps 拦截优先级与物理 Ctrl 守卫。
- **复测（2026-10-07，HEAD `c23a42d`，D26）**：**27** 条记录 ⇒
  **CAUGHT 26 / EQUIVALENT 1 / SURVIVED 0 / ERROR 0**，退出码 0。新增 5 条 D26 变异
  （`d26-no-compensate` / `d26-always-compensate` / `d26-typed-mark-dropped` /
  `d26-esc-passthrough-compensate` / `d26-replay-mark-dropped`）全部 CAUGHT；
  同时把 `d25-rec-overflow-guard` 的 `old` 锚点更新到 D26 重写后的 `rec_commit_insert()`。
- **门禁第一次运行就查出一个真实测试空洞（已按本节"补齐覆盖后重测"处理）**：
  `budget-clamp-n-floor-zero`（`kv_emit_clamp_n` 的 `maxn < 1 → 1` 下限改成 `0`）首轮
  **SURVIVED** —— 既有预算测试只预填到 240（room=10），**从未**走到 `room < fixed+per`，
  于是"预算不够时至少做 1 次、绝不半截"这条 design §4.4 契约无人钉住。已在
  `test_main.c:1686` 补 6 条断言（预填 249/room=1 时 `99X`/`99X`（前删）应发 251/252
  键且尾部改文档键仍在），该变异随即 **CAUGHT**（`pass=784 fail=0`，§2①同步更新）。
  等价记录 `d19-outer-clamp-idempotent`（外层夹取与内层同名调用幂等）的论证见记录表 `why`。

### 1.2 与真实 Vim 对照（ground truth）

- 唯一基准：**`/usr/bin/vim.tiny`**（本机 VIM 9.1、无 `+eval`；`vim.tiny --version` 实测
  `Included patches: 1-948, 950-1230, 1242, 1244`）。
- 调用方式（`engine/test/host/kvhost.py: vim_run()` 已实现，改动 harness 时必须保持）：

  ```sh
  vim.tiny -Nu NONE -N -es \
    -c 'set nofixendofline' \
    -c "silent! normal! <KEYS>" \
    -c 'w! OUT' -c 'qall!' IN
  ```

  要点：
  1. `<KEYS>` 里的 Esc 必须是**真 `\x1b` 字节**，不能是 `\e` 两个字面字符（历史 harness 缺陷 H1
     曾因此让所有 Esc 用例的基准出错）；
  2. 必须 `silent!`（键序列中途报错不能中断写入）；
  3. 必须先用 `gg` 定位：`-es` 下光标从**末行**开始；
  4. 写回用 `w!`，并显式 `set nofixendofline`，避免末行换行被自动补上。
- 三个比较维度：**缓冲区**、**无名寄存器**、**光标**（`engine/test/host/kvhost.py`）。任一不符即
  该例不通过；能对上 Vim 却挂在 XFAIL 表里 = XPASS（见 §3）。

### 1.3 缺陷编号与「已知偏差」引用链

- **缺陷编号**：修复类缺陷用 `D1…D27`（`vim/changes.md` §7.14–§7.47 逐条登记）；**固有/声明类
  偏差**用独立编号（现有：`E-W`、`YCOL`、`VCUR`、`VPASTE`、`PASTEC`、`EOLDEL`、`GPFX`、
  `FAILMOT`、`D2`、`D14`–`D18`、`D23`、`VCAP`、`VBLOCK`、`IND`、`XEMPTY`、`PEMPTY`，以及
  `design.md` §4.9 的 ①–⑩）。编号一经使用**不得复用/改义**。
- **引用链（可审计）**：
  `matrix.py: XFAIL[用例名] = 偏差编号` → `matrix.py: DEVIATIONS[编号] = 一句话 + design 章节`。
  每个 xfail 都必须写编号；判据见 `engine/test/host/matrix.py:439`（XFAIL 表）与
  `engine/test/host/matrix.py:405`（DEVIATIONS 表）。
- **出处顺序**：先在设计文档写清（`design.md` §4.4/§4.9 或 `readme.md`），再在 `DEVIATIONS`
  里引用它；不得只在测试代码里"口头声明"。
- 如需新增编号，**先改文档**（本节 + `design.md` 对应表），再改 `matrix.py`。

### 1.4 独立复核纪律

- 涉及 P0/P1 或跨键盘的改动，由**未参与实现**的一方独立复核（`qmk/README.md` §7），并满足：
  1. **前/后两个探针**（修改前的提交 vs 修改后）逐例对照**同一批用例**：
     - 能比发射流的场合**逐键比对**发射流（最严格）；
     - 不能比发射流的场合**逐维比对**缓冲区/寄存器/光标；
  2. 结论必须包含 **`N FIXED / 0 REGRESSED`**：`REGRESSED > 0` 即视为未完成，先修回归；
  3. 数字必须实测（`vim/changes.md` §7.24–§7.30 的 `44 FIXED / 0 REGRESSED`、
     `22 FIXED / 0 REGRESSED` 等均为此格式）。
- **子代理结论必须由父级独立复验后才采信**。近期实测教训：两处子代理结论被复核推翻 ——
  `Ncc` 计数夹取"不符"实为 harness 被并发进程扰乱（`changes.md` §7.26），`y` 的光标语义
  "Vim 的 `y` 不动光标"是错的（`changes.md` §7.23 P1-3）。
- **harness 自身也要审**：`/tmp/audA` 旧 harness 有 7 个自身缺陷，其中"H1 把 `\e` 当字面量"
  会让**所有** Esc 用例基准出错。

---

## 2. 验收基线（"什么算绿"）

全部在 **`engine/`** 目录下执行。任一条不满足 = 未完成。四条可**一条命令一次跑完**（§2.1）：
`make -C engine verify-all`。

| # | 命令 | 判据 | 实测（2026-10-01，HEAD `927ecb1`，工作区干净） |
|---|:---|:---|:---|
| ① | `make test` | `fail=0`，退出码 0 | `pass=784 fail=0`（补齐 §1.1.1 发现的预算下限空洞后：原 778 + 6 条断言） |
| ② | `make glue-test` | **10** 个套件全部 `fail=0` | 见下表，合计 2592 断言 |
| ③ | `make matrix-test` | 退出码 0；`KNOWN-FAIL 0`、`NEW-FAIL 0`，且无 XFAIL 条目沦为 XPASS | `TOTAL 598 PASS 405 XFAIL 193 KNOWN-FAIL 0 NEW-FAIL 0`，退出码 0 |

**② 的逐套件实测**（`make glue-test` 输出，10/10 `fail=0`）：

| 套件 | 断言 | 套件 | 断言 |
|:---|---:|:---|---:|
| `test_glue`（glue） | 671 | `test_modifiers`（modifiers） | 441 |
| `test_glue_falsify`（glue-falsify） | 250 | `test_rgb`（rgb） | 377 |
| `test_visual_passthrough`（visual-passthrough） | 68 | `test_modifier_falsify`（modifier-falsify） | 493 |
| `test_pending_clear_probe`（pending-clear） | 24 | `test_adapter_regress`（adapter-regress） | 48 |
| `test_strict_clear_falsify`（strict-clear-falsify） | 153 | `test_adapter_regress2`（adapter-regress2） | 67 |

> **复测（2026-10-01，HEAD `05c3ad5`，`make verify-all` 全绿）**：表内是 HEAD `927ecb1` 的时点值；
> 中间提交增补断言后已有两个套件变化 —— `test_glue` 671 → **681**、`test_rgb` 377 → **414**，
> 十套件合计 2592 → **2639**（其余套件不变）。判据不变：**10 个套件全部 `fail=0`**。

> **复测（2026-10-07，HEAD `c23a42d`，D26 落地后；四条门禁逐条手跑）**：表内仍是 `927ecb1` 的
> 时点值，D26 后实测为 ——
> ① `pass=825 fail=0`（D25 的 784 + D26 新增 41 条断言：`test_leave_insert_left`、
> `test_dot_repeat_insert` 的回放 Left、`test_count_queue_accumulation` 第 (7) 段）；
> ② 10/10 `fail=0`，逐套件 `760/250/68/24/153/441/414/493/48/67`，合计 **2718**
> （`test_glue` 681 → **760**，其余同上一条复测注）；
> ③ `TOTAL 613 PASS 420 XFAIL 193 KNOWN-FAIL 0 NEW-FAIL 0`，退出码 0（新增 15 条 D26 用例；
> XFAIL 表**不增行**，只把 6 条 `ins-o/O-*` 的引用由 D18 改为新增的 `XEMPTY`/`PEMPTY`）；
> ④ `27` 条记录 ⇒ **CAUGHT 26 / EQUIVALENT 1 / SURVIVED 0 / ERROR 0**，退出码 0
> （新增 5 条 D26 变异全部 CAUGHT；等价项仍是 `d19-outer-clamp-idempotent`）。

> ⚠️ **`make glue-test` 会改写两个已跟踪的二进制**：`engine/test/glue/test_adapter_regress`
> 与 `engine/test/glue/test_adapter_regress2`（`engine/Makefile:21-27` 对 `GLUE_TESTS`
> 逐个 `-o test/glue/$$t` 覆盖）。
> 收尾必须 `git checkout -- engine/test/glue/test_adapter_regress engine/test/glue/test_adapter_regress2`
> 还原，且**不得暂存**（`git status --porcelain` 必须干净）。
> 其余套件二进制在仓库根 `.gitignore:1-12` 里，不会被跟踪。
> 经 `make mutation-test` 驱动时由 runner 自己还原（§1.1.1），手工跑 `make glue-test` 才需要人工还原。

**③ 的输出细节**：`matrix.py` 只在**非空**时打印 `XPASS ...` / `NEW FAILURES ...` / `FIXED ...`
行（`engine/test/host/matrix.py:688-711`），所以"绿"的输出里**没有** `XPASS 0` 这个字样；
判绿看**退出码 0** 与 `KNOWN-FAIL 0 NEW-FAIL 0`，以及没有 `XPASS (in the xfail table ...)` 行。
退出码公式：`return 1 if (unknown or xpasses or fixed) else 0`（`matrix.py:724`）。

**④ 变异验证**（`make mutation-test`，§1.1.1）：**27** 条记录全绿 ——
`CAUGHT 26 / EQUIVALENT 1 / SURVIVED 0 / ERROR 0`，退出码 0（D26 复测，2026-10-07，HEAD `c23a42d`）；
判据、记录格式与"如何加一条"见 §1.1.1（记录表 `engine/test/mutants/mutants.txt`）。

### 2.1 一条命令跑完全部门禁 `make verify-all`

```sh
make -C engine verify-all                                  # 顺序固定，首个失败即停
make -C engine verify-all VERIFY_GATES="test glue-test"    # 只跑子集（自检用）
```

- **顺序（固定）**：`test` → `glue-test` → `matrix-test` → `mutation-test`；**首个失败即停**，
  其后的门禁在汇总表里记 `SKIP`；汇总表列为 `gate | result | wall time`，末尾打印**总墙钟时间**。
- **退出码**：`0` = 全绿；非 0 = 未通过。内部 shell 用 `1`（门禁失败）/`2`（收尾自证失败、拒绝报绿）
  区分两类失败，但 make 对任何失败配方都返回 `2`，故对外只保证"非 0"；失败类别看汇总后的
  `[verify-all]` 行。
- **收尾还原（只碰这两个文件）**：`make glue-test` 会改写 §2② 警告的那两个**已跟踪**二进制；
  `verify-all` 在 `glue-test` 之后与全部门禁跑完后各做一次
  `git checkout -- engine/test/glue/test_adapter_regress engine/test/glue/test_adapter_regress2`。
- **收尾自证（与 `test/mutants/run.py` 同口径）**：
  - 运行前后各取一次 `git status --porcelain`；还原上述两个二进制后，若出现**新增的跟踪改动** ⇒
    拒绝报绿（非 0 退出）并逐个列出文件名；
  - 新出现的**未跟踪**文件（实测 `make matrix-test` 期间 `vim.tiny` 会短暂生成
    `engine/.swp`/`.swpx`）**只提示、不判失败**；
  - 运行前**已存在**的其它跟踪改动**一律不碰**，只在汇总后提示 —— 因此 `verify-all` 可以安全地
    在脏工作区上运行，不会吞掉用户未提交的工作。
- **实测（2026-10-01）**：四道门禁 **4/4 OK**，判定数字与 §2①②③④ 完全一致 ——
  `pass=784 fail=0`、10/10 `fail=0`、`KNOWN-FAIL 0 NEW-FAIL 0`、
  `CAUGHT 15 / EQUIVALENT 1 / SURVIVED 0`。
  - **工作区干净（HEAD `05c3ad5` = 引入本目标的提交）**：退出码 **0**；`test` 1.7 s /
    `glue-test` 12.5 s / `matrix-test` 209.3 s / `mutation-test` 45.3 s，
    **总墙钟 268.8 s**（matrix-test 占大头）；无任何提示行，收尾 `git status` 干净。
  - **脏工作区（HEAD `35b24d2` + 未提交的跟踪文档改动）**：四道门禁同样 4/4 OK（总墙钟 263.0 s），
    但该次运行期间我在并行编辑跟踪文档，收尾自证检测到"新增的跟踪改动" ⇒ **拒绝报绿**；
    运行前已存在的改动未被触碰。即"门禁结果"与"收尾自证"是两件事，自证项确实生效。

---

## 3. 矩阵测试与棘轮基线的语义

`make matrix-test`（`engine/test/host/matrix.py`）把引擎发出的**宿主键码流**喂给
`engine/test/host/kvhost.py` 的宿主编辑器模型，再与真实 `vim.tiny` 逐例比较（§1.2 三维度）。

### 3.1 门禁是**严格 xfail**

- 用例一旦列在 `XFAIL` 里、却与 Vim **完全一致** ⇒ 判**失败（XPASS）**，要求**删除该条**
  （`matrix.py:659-660`、`709-711`、`724`）。
- **为什么**：否则 XFAIL 表会腐烂 —— 已修好的缺陷被"仍在坏"的表掩盖，门禁变成橡皮图章。
- 因此 `XFAIL` 表与 `known_failures.txt` **都只能变小**，不能为了让门禁变绿而加行。

### 3.2 `engine/test/host/known_failures.txt` 是**棘轮基线**

格式 `<用例名> <失败维度>`，维度 ∈ `buf` / `reg` / `cur`（多维度用 `+`，如 `dw buf+reg`）。
三种硬失败（`matrix.py:668-701`、`724`）：

| 情况 | 判定 |
|:---|:---|
| 某用例**不在**表里，且与 Vim 不一致 | **硬失败**（新回归 / 未声明的缺陷） |
| 某用例在表里，但**多出**未记录的失败维度 | **硬失败**（已知坏用例里出现了新的损坏） |
| 表里的用例现在**完全一致** | **硬失败**，要求删除该行（表只能变小） |

**当前状态**：该表**已排空**（只剩头部说明；实测 `KNOWN-FAIL 0`），即所有不符用例都已逐条引用
`DEVIATIONS` 的书面声明 —— 门禁现在等价于**严格 xfail**。以后不应再往表里加行为未理解的条目；
确实无法引用声明的，先在文档里声明并登记编号。

### 3.3 重名陷阱（判定必须按"该名字是否还有任何不符实例"）

少数用例名重复（同一名字、两个不同缓冲区）。按名字建索引的 XFAIL/基线会把
"一个实例通过、一个实例不符"**误判成 XPASS**（进而错误地要求删表）。

- 正确判据：`names_failing = {所有失败实例的名字}`；只有当某名字**不在** `names_failing` 中
  （即**没有任何**实例还在失败）时才算 XPASS（`matrix.py:652-655`、`676-680`）。
- 新增用例时**名字必须唯一**：曾因 `'yl l p'` 与 `'yllp'` 都生成 `pp-yllp`，导致后者被**隐藏**
  （一个通过、一个不符）；现在生成器用 `_` 转义空格保证唯一（`matrix.py:355-359`）。

### 3.4 无效用例（含字面空格键）

含**字面空格键**的用例（如 `'yl l p'`）测的是**宿主模型**（模型未实现"空格作为移动"）
而不是引擎，**必须剔除**。判据：用例的键序列里出现字面空格 ⇒ 不是引擎行为，删掉
（`matrix.py:355-357` 的实现注释）。

---

## 4. 键码预算规范（键码是正确性资源，不只是速度）

### 4.1 事实

- 发送队列 `EMIT_CAP = 256`（`engine/src/emit.c:3`，`static kv_keycode_t s_buf[256]` = **512 B**
  RAM，`engine/src/emit.c:5`），**满格时 `return` = 静默丢键**（`engine/src/emit.c:22`）。
  丢的是**命令尾部** —— 可能出现"范围已选中、却发不出最后那个真正改文档的键"的半截执行。
- `KV_CMD_KEY_BUDGET = 250`（`engine/src/command.c:12`）是同一道闸的正常工作上限（留 6 格余量）。
- 节流 `KV_EMIT_GAP_MS = 1`（`engine/src/emit.h:8`，1 键/ms）。

### 4.2 规范（所有**带计数**的发射器）

1. 发键前先用 `kv_emit_room()`（`= KV_CMD_KEY_BUDGET − kv_emit_pending()`，下限 0，
   `engine/src/command.c:15`）取剩余预算；
2. 按**自身每次重复成本**截断计数：`kv_emit_clamp_n(n, fixed, per)`（`command.c:24`）保证
   `fixed + per×n ≤ room`，下限 1；几何方向键组用 `emit_taps_room()`（`command.c:36`）预扣
   本组之后必然要发的固定键数；
3. 两条不变式：**(a) 整条命令一定发完**（尤其最后那个改文档的键，如 `Ctrl+X`/`Ctrl+V`/`Tab`）；
   **(b) 本次发完后队列绝不到顶**（`pending ≤ 250 < 256`）；
4. 剩余预算连固定骨架都放不下时，重复数取 **1**（宁可少做，绝不做半截）。

### 4.3 上界与实测

- 单命令键码上界以 **`v99w`** 为准，**不是** `99J`：实测（前后探针对照，`probe`）：

  | 命令 | 修前 `760b83a` | 修后 `7be99d1` |
  |:---|:---|:---|
  | `v99w` | `nkeys=257`、`maxpend=256`（**已封顶在丢键**） | `nkeys=250`、`maxpend=249` |
  | `99J`（连接） | 248 | 248 |
  | `99dd` / `99yy` | 103 / 202 | 103 / 202 |
  | `99x` / `99X` / `99s` | 100 / 101 / 100 | 100 / 101 / 100 |
  | `99C` / `99D` / `99Y` | 101 / 101 / 202 | 101 / 101 / 202 |

  （`v99w` 含进入 Visual 的 1 键；`99J` 是 4 键/次、截断到 62 次 = 248。）
- **`pending = 0` 的单条命令逐键不变**：实测把 `760b83a` 与 `7be99d1` 分别编成探针，在
  **矩阵全部 598 条用例**上逐键比对发射流 ⇒ **598/598 逐字节相同**；唯一变化是 `v99w`
  （257 → 250，本来就在丢键）。
- **已知残余**：若队列已被预填到 `pending ≥ 248`，剩余预算连最小骨架都放不下，按"重复数取 1"
  仍会把队列顶到 256，可能丢掉尾部的**纯装饰键**（改文档的键仍在队列内）。真实 1 键/ms 的排空
  节奏下**不可达**；彻底消除需要"room 不足则整条不发"，与规范第 4 条冲突。

---

## 5. 体积/效率评估结论（避免重复劳动）

> 结论分两类：**已实测**（可直接引用）与**未复验**（引用前必须重测）。评估一律以
> **源码 + 指定工具链 + 显式命令行**为准，禁止把一侧结论外推到另一侧。

### 5.1 已实测

- **两侧构建配置不同**：QK61 `LTO_ENABLE = yes`（`qmk_firmware` `qk61` 分支
  `keyboards/qk61/keymaps/vim/rules.mk` 末尾）；NUT65 的 `rules.mk` **没有** `LTO_ENABLE`。
  ⇒ **体积评估必须两侧各自重建 `cmp`**，不能把 LTO 侧的结论外推。
- **`classify.c` 里没有任何 `static const` 表**（`grep -c "static const" engine/src/classify.c`
  = 0；实现是纯 `switch`，"查表冗余"的怀疑不成立）。
- **`% 256` 在 Cortex-M0 上不是掩码**（本次实测，**与"收益为零"的旧结论相反**）：
  - 真实 QK61 固件 `.build/qk61_vim.elf` 里 `push` 是 **64 B**（`0x4a3c`，含
    `ands r3,r5` + `bpl` + `subs`/`orrs`/`adds` 的符号修正序列）；
  - 把 `engine/src/emit.c` 的两处 `% EMIT_CAP` 换成 `& (EMIT_CAP-1)` 后，同一 flags 下
    `push` 降到 **44 B**（`uxtb` 掩码），`send_one` 由 80 → 60 B；
  - 引擎源码集单独链接（`-Os -mcpu=cortex-m0 -mthumb -ffunction-sections -fdata-sections
    -fno-common -fshort-enums`，`-Wl,--gc-sections`，含 libgcc 与一个仅用于保留符号的微型
    driver `main`；**绝对值无意义，只看同口径差值**）：`.text` **6732 → 6696 B（LTO）**、
    **7732 → 7692 B（无 LTO）**。
  ⇒ 该候选**不是**零收益；引用"源码级微优化收益为零"时必须重新验证（至少 `%`→`&` 已被推翻）。
- **真正的杠杆是键码数量**（既是延迟也是安全裕量，见 §4）；`99yy`/`99Y` 恒为 202 键，
  `v99w` 是 250 键 —— 任何新增"每重复多键"的实现都会直接吃掉预算。
- **疑似死代码已清理**：`kv_emit_yank_to_eol` / `_n` 已由 `b84d09d`（0 调用者）删除，当前源码中
  不存在（`grep -rn yank_to_eol engine/src` 无命中）。"删死代码省几十字节"是**历史实验**，现在无
  对象可删。

  **已实现并发布（父级复验，固件级实测）**：`&` 替换于 `66d3db2` 落地，两侧固件各自重建并与上一版归档 `cmp` 对比的**真实差值**：**NUT65 −16 B**（83308 → 83292）、**QK61 −40 B**（77548 → 77508）。注：`&` 的等价性由编译期静态断言锁死（`emit.c` 的 `kv_emit_cap_must_be_power_of_two`），被除数恒非负，故与 `%` 严格等价。

### 5.2 未复验（引用前必须重测）

- "四个候选（单次 `classify` 复用、删死代码、`&` 替 `%`、合并重复 case）链接出的 `.text`
  逐字节相同（6488 B）"与"合并重复 case 反而 +4 B"：本次**未能复现**（`&` 一项已被 §5.1 推翻），
  且 `6488 B` 的测量口径（是否含 glue/是否含 libgcc）未记录。**不要再把它当事实引用**。
- "NUT65 未开 LTO 时删同一死代码省几十字节（−20 B）"：**已由父级复验为真，可直接引用** ——
  归档物本身就构成证据，无需重建：`git show v2.39-nut65:output/leku_nut65_vim_v2.39.bin | wc -c`
  = **82860**，`git show v2.40-nut65:output/leku_nut65_vim_v2.40.bin | wc -c` = **82840**，
  差值即 **−20 B**（v2.40 那次提交的唯一源码改动就是删除 `kv_emit_yank_to_eol`/`_n`）。
  教训：**"无法复现"要先检查归档/标签里是否已经有证据**，别急着标"勿引用"。
- 固件整体体积：本次只读测量了**既有** `.build/qk61_vim.elf`（构建于 2026-09-30 18:31，
  其子模块工作区为 `7be99d1`）：`text 76232 / data 1316 / bss 15040` B；引擎+glue 符号
  （按符号名统计，**不含量化到 `kv_kbd` 的内联静态函数**）约 **7002 B / 39 个符号**。
  未重建、未做 `.text` 快照。

### 5.3 不要改动的清单（看着浪费、实则正确性关键）

| 项 | 位置 | 去掉/缩小的后果 |
|:---|:---|:---|
| `Home×2` | `command.c` 行操作/缩进 | 抵消宿主的 smart-home；省 1 键即让 `dd`/`yy` 的行范围错 |
| D19/D24 的 `Esc, Shift+Left` 翻锚点 | 字符级 VISUAL | 半开选区会塌空 → `Ctrl+X` 退化成"剪切整行" = **数据损坏** |
| `y` / `>>` 的 `Up×n` 光标回位 | `command.c` | 真实 Vim 的 `y` 不移动光标；不回位则后续 `x`/`p` 作用错位置 |
| `J` 的 4 键（`End,Space,Delete,Left`） | `kv_emit_join()` | `Left` 不可省：Vim 的光标停在新插入的空格上，去掉后 `Jx` 删错字符 |
| `EMIT_CAP = 256` / `s_buf` 512 B | `emit.c` | 由 248/250 的预算决定；减容 = 静默丢键 |
| `KV_EMIT_GAP_MS = 1` | `emit.h` | 宿主兼容性节流，改它属**时序行为变更**（需真机重验） |

---

## 6. 发布流程（共享层 → 键盘分支）

操作细节（子模块 `fetch`/`checkout`/`git add`、编译归档路径、验证清单、坑位）见
[`README.md`](README.md) §3–§6。此处只固化**判据与硬性要求**：

1. 共享层改动 **push 后**，在键盘仓库（`/home/springremember/fnos/home/project/myqmk/qmk_firmware`，
   分支 `nut65` / `qk61`，remote 是 **`newfork`**）依次：bump 子模块指针 → 重建 → 归档
   `output/<kb>_vim_vX.Y.{bin,hex,_via.json}` → readme 版本块加"当前/上一版" →
   `git tag vX.Y-<kb>` → push **分支与标签**。
2. **只有 `engine/src/` 变了才重发版本**。仅测试/文档变更**只同步子模块指针**（固件字节不变），
   并在提交信息里说明理由。
3. **归档必须逐字节校验**：与 `.build/` 的产物 `cmp`；把归档的 HEX 解码回来与 BIN 比对；
   `git ls-remote` 复核分支与标签都已 push。
4. **两个键盘的 `output/` 不得交叉污染**（`qmk/README.md` §5④ 的两个 `grep -c` 必须为 0；
   归档文件名带 `<kb>` 前缀）。
5. **提交信息用 `git commit -F <file>`**：CJK + 括号走 `-m` 会被 shell/编码搞坏。
6. **不要引入 `sync-keyboards.sh`**（用户明确不要）：同步改用本文件 + `qmk/README.md` 的
   文档规范，手工按 §3 执行并逐条核对。
7. 发布提交与实现提交**分开**；tag 用注解 tag。

---

## 7. 硬件/真机未验证项（必须原样保留的清单）

以下是**主机测试无法覆盖**的假设/行为，任何"全绿"声明都不得声称已覆盖它们。改动相关代码后必须
在真机上重验，并把结论写回本节或对应键盘 readme。

> **逐条可执行版本见 [`on-device-checklist.md`](on-device-checklist.md)**（编号 A1–A11）：
> 每条都给了 编号 / 要验证什么 / 前置条件 / 操作步骤 / **预期结果（具体到行数、字节数、颜色）** /
> 风险 / 结果栏（☐ 通过 ☐ 不符）。本节 1–7 是原有条目；8–11 是随该清单**补记**的同类未验证项
> （原清单遗漏，编号 1–7 不动，避免破坏既有引用）。

1. **真实 OS/IME 下的右 Shift 懒 Shift**：孤立右 Shift 不发键（避免 IME 切换）、与其他键同按才
   临时补左 Shift —— 不同 IME（尤其 Linux fcitx / macOS）行为未被真机验证。
   （操作步骤：清单 **A2**）
2. **Caps 合成 Ctrl 与物理 Ctrl 的交错**：位模型（LCTL/RCTL 是不同 bit、`register_code`
   无引用计数）已有主机覆盖（`engine/test/glue/test_rgb.c: test_caps_ctrl_bitmodel`，
   `caps/testcase.md` §0 列为已覆盖），但"合成 Ctrl + 物理 Ctrl + 多个非 F 键重叠"在真机上的
   组合观感未验证。（操作步骤：清单 **A3**）
3. **宿主 `Shift+↓` 的列保持**：行选/字符级 VISUAL 的 `Shift+↓` 近似**假设宿主保持列**；
   VSCode / 终端 / 浏览器等宿主是否如此未逐一验证。（操作步骤：清单 **A4**）
4. **7 色在真实矩阵上的实际观感**（紫/洋红的区分、橙 vs 绿的可辨识度、亮度）。
   （操作步骤：清单 **A8**）
5. **NUT65 深睡 / 无线 / bootloader**：`Fn+L` 短按休眠、`Fn+右上角` 唯一唤醒、
   `Fn+右Shift+Esc` bootloader 在真机上的行为。（操作步骤：清单 **A9**）
6. **QK61 USB 枚举与 16 KB RAM 余量**：`>~0x13F58(81752B)` 会导致有线 USB 枚举失败
   （`qmk/README.md` §4）；既有 ELF 实测 `bss = 15040 B`（< 16 KiB，余量约 1.3 KiB，且该值随
   功能增长而变），每次改动后必须看 `Size after:`。（操作步骤：清单 **A10**）
7. **真机宿主对连续键码的接受速率**：`KV_EMIT_GAP_MS = 1`（1 键/ms）是**假设**；大计数命令
   （如 `99yy` 202 键）在真实宿主上是否被完整接受、是否丢键未验证。（操作步骤：清单 **A1**；
   尤其 §4.3 的「已知残余」= 队列预填到 `pending ≥ 248` 时连发两条大命令）
8. **`Ctrl+C` 复制后宿主是否保留选区**：`y`/`yy`/行选动作在 `Ctrl+C` 后补发 `Esc` 的前提是
   "宿主复制后仍保留高亮选区"（`vim/design.md` §4.9「`Esc` 收尾」）；真实宿主是否如此、
   `Esc` 是否另有副作用未验证。（操作步骤：清单 **A5**）
9. **末行无尾换行的缓冲区表现**：`dd` 依赖 `Shift+Right` 在**缓冲末尾**是 no-op
   （`design.md` §4.9 ③），主机模型用 `set nofixendofline` 复刻；真实编辑器（多数会保存时自动补
   尾换行）是否一致未验证。（操作步骤：清单 **A6**）
10. **3 秒 `Esc` 宽限（grace）的实际手感**：`VIM_ESC_GRACE_MS = 3000` 的窗口长度、橙色续期、
    连续 `Esc` 场景的可用性，以及 >65.5 s 不出现 16 位回绕假橙，都只在主机侧验证过。
    （操作步骤：清单 **A7**）
11. **刷机前归档核对**：待刷的 `output/<kb>_vim_vX.Y.bin` 是否确实来自对应分支、版本块/注解 tag/
    子模块指针/`cmp`/HEX↔BIN/sha256 是否一致，是纯人工步骤（`qmk/README.md` §4/§5）。
    （操作步骤：清单 **A11**）

---

## 8. 本文件的维护规则

- 本文所有数字**必须**能被复现：命令 + HEAD + 时点三者齐全；数字变化时**同一提交**内更新。
- 与 `qmk/README.md` 冲突时：**流程/同步细节**以 `qmk/README.md` 为准，**判据/基线/评估结论**
  以本文为准；发现冲突先改文档再改实现。
- 本文的"不要改动清单"是**设计约束**（不是建议）：如确需改动，先在 `design.md` 写清正确性论证
  并给出前后探针 `0 REGRESSED` 的证据。

### 6.1 归档完整性：发布后必须做的三项核对（2026-09 补强）

一次实测发现 **qk61 分支 v1.0 与 v2.0–v2.7 的 `output/*.hex` 与同目录 `*.bin` 不是同一张镜像**
（共同范围内约 3000 字节不同、长度差 2 字节）—— 同一版本的两个归档互不一致，且**事后无法判断
哪份才是当年实际发布的那份**。v2.8 起（含当前版本）全部一致，NUT65 全历史一致。

因此发布归档后**必须**逐项核对，缺一不可：

1. **HEX ↔ BIN 回解一致**：把 `.hex` 解码回字节流，必须与同版本 `.bin` **逐字节相同**。
   （`cmp` 只能证明「文件与构建目录一致」，不能证明这两个归档彼此一致。）
2. **与构建产物一致**：`.bin`/`.hex` 都要与 `.build/` 下同名产物 `cmp` 相等。
3. **`_via.json` 与上一版逐字节相同**（历次实测其 md5 不变）；若确有变化必须在提交信息里说明原因。

**教训**：本项目的流程文档里本来就有第 1 项，但两次发布（NUT65 V2.43 / QK61 V2.36）我只做了
`cmp` 就放行 —— 全绿只是运气（事后回查这两版确实一致）。**流程里写了不等于做了**；
`qmk/on-device-checklist.md` A11 已把这三项列为刷机前核对项。
