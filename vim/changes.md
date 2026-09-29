# 变更记录 — vim 引擎

> 记录 vim 引擎（qmk-vim fork）的变更、历史问题与最终取舍。
> 目标设计见 [`design.md`](design.md)；对应测试见 [`testcase.md`](testcase.md)；使用见 [`readme.md`](readme.md)。

## 1. 版本

- **V1.0（冻结）**：引擎代码 commit `62bb338`；固件另存 `output/*_v1.0.*`，避免被后续重构覆盖。
- 重构目标：见 [`design.md`](design.md)（按模式解耦 + token 队列 + 表驱动 + pending 严格清空）。
- **V2.0（进行中，含 2026-09-27 审核补丁）**：`engine/` 核心层已落地（与 QMK 解耦的纯 C + 主机单测），
  里程碑 1–8 对应 `design.md` §6；`make -C engine test` 全绿。
  接回固件（替换 `process_func`）属后续阶段。
- **V2.0 架构修订（2026-09-22 全面审查后）**：
  - `kv_kbd` 返回 `kv_result_t`（CONSUMED/PASSTHROUGH），透传责任方=调用方（§4.7）；
  - **任何模式/使能切换一律清 pending 且清 repeat 录制缓存**（`s_last` 保留），`kv_enable` 固定从 INSERT 起（§4.7）；
  - Visual 明确**无 pending**（非法键吞键）；MOUSE 明确"`Shift` 不退出；`Ctrl`/`Alt`/`GUI` 按下退出并重识别；其它键退出强制释放鼠标键"（§4.9）；
  - **新增 §4.12 glue 层规格**（`qmk/` 共享适配层：统一配对表、物理修饰键影子、held motion、极性封装），keymap 禁止复制实现。

### V2.0 补丁（2026-09-27，独立审核驱动）

- **可视为行（`V`）**：进入发 `Home`+`Shift+End`（选中整行）；`w`/`e`/`b` 由词级改为**整行推进**；
  `d`/`y`/`c`/`s` 对整行生效；**动作后退出可视**（`y`/`d`/`x`→NORMAL，`c`/`s`→INSERT，`p`→NORMAL）。
- **状态色**：由六色扩为**七色**，`Visual-Line` 独立为**洋红 rose `#FF0080`**（与 Visual 紫 `#800080` 区分）。
- **可视模式计数**：上限 2 位（第 3 位忽略）、`0` 在计数中作数字（`10j` 合法）、非数字键**立即消费**计数、
  `gg` 已实现（`Ctrl+Shift+Home`）、`G` 丢弃计数；可视输入在**所有截断路径**（透传 / CAG / myfn 吞键）作废。
- **Caps 模式审计修复**：重入先清理（修宿主键永久卡住）、held 表溢出改为"不注册不消费"、
  release 按实例过滤、物理 Ctrl 中途松开后补注册、层键豁免并放行、孤立 release 守卫、
  Ctrl 归属跟踪（不误卸物理按住）。
- **配色/提示**：`Normal --Esc--> Insert` 后 3s 内 Esc 灯与电量灯显橙 `#FF8000`；窗口计时用 **32 位**
  （修 16 位回绕导致的假命中）。

---

## 2. 相对上游的功能修改

- `gg` / `G` 改为 `Ctrl+Home` / `Ctrl+End`（比上游的 `Ctrl+A`+方向更可靠）。
- `o` / `O` 换行改用 **`Shift+Enter`**（部分编辑器 `Enter` 行为不一致）。
- `dd` 行删除重写（见 §5 E1）：`Home×2 → Shift+End → Ctrl+X → Backspace`。
- `yy` 行复制：`Home×2 → Shift+Down×n → Ctrl+C`（`Home×2` 抵消 smart-home）。
- 操作符待定遇非法键：**清空 pending 并把该键重新识别**（非 vim 键透传）——取代上游 `g` 前缀 / 文本对象的吞键（上游操作符本就重处理）。
- 计数：仅前缀、**前后相乘**（`2d3w`=`d6w`）；最多 **2 位（≤99）**，第 3 位起忽略；作用域限移动/缩进/行操作（含 `cc`/`S`），其余键**丢弃计数**；其中移动的例外 `G`/`gg` **任何上下文都丢弃计数**（见 §3 A1）。
- **丢弃计数的键**：`G`/`gg`（移动例外，任何上下文）、`C D Y X`、`x s p P J u .`、`v V`、插入键、`ZZ`——计数被吸收但不生效。`S`≡`cc` 接受计数。
- Visual-Line 首次 `j` 用 `Home` 折叠（**V1.0 历史行为**，避免在 VSCode 中 `Left` 跨行丢行）；新设计为**编辑器无关**，不含该特化。
- 插入模式不再无条件 `clear_keyboard()`（见 §3 A3 的目标）。

---

## 2b. Esc / Caps / 右 Shift 行为变更（V2.x）

- **Esc 切换**：Insert 按 `Esc` 改为**吞键进 Normal（不发 Esc）**；Normal 空闲按 `Esc` 发真实 `Esc` 并回 Insert。
  引擎 `kv_kbd()` 对 Insert 一律 `KV_PASSTHROUGH`，Esc 切换移交共享 keymap 层步骤 6 `esc_process()`。
- **Esc 宽限窗口（3s）**：仅由「Normal 空闲 Esc → Insert」开启；窗口内 Insert `Esc` 仍发真实 Esc 并重置计时；
  窗口外 Insert `Esc` 进 Normal。其它进入 Insert 的路径（开机/Caps 开 vim/编辑命令）**无宽限**。
- **Caps 触发（现行）**：**`Fn`+`Caps` 单击 = 开关 vim**；**裸 `Caps` 单击无任何效果**；`Caps` **按下即进入
  Caps 模式**（不等 200ms，见 [`../caps/changes.md`](../caps/changes.md)），按下期间未按其它键就抬起则撤销。
  `Caps` 始终被消费（永不作 Caps Lock）。历史上曾为"单击开关 vim / 长按临时 Normal"，均已废弃。
- **右 Shift 懒发送**：vim 开启时孤立右 Shift 不发键（避免宿主输入法切换）；与它键同按才临时补左 Shift
  （`右Shift+a`=`A`、`右Shift+Ctrl+C`=`Ctrl+Shift+C`）；`右Shift+Esc` 仍输出裸 `` ` ``；vim 关闭时右 Shift 正常。
- **回到打字提示色（V2.x 新增）**：`Normal` 空闲 `Esc` 回到 `Insert` 后 3s 内，模式指示色由 Insert 绿替换为
  `cfg.insert_flash_color`（两键盘均配橙 `#FF8000`），随后自动恢复。判据 `vim_insert_flash()` = vim 开 +
  Insert + 上述宽限窗口未过期；色值裁决在共享层 `vim_insert_flash_color()`（`0`=不覆盖）。窗口内 `Esc` 续期；
  其它进入 Insert 的路径不亮橙。**该窗口计时改 32 位**（见下条）。
- **修复：Esc 宽限窗口 16 位计时回绕（P1）**：窗口戳原先存 `timer_read()`（`(uint16_t)timer_read32()`），
  65536ms 后 `elapsed` 回绕为 0，使**已过期**的窗口重新被判有效——持续在 Insert 打字每 65.5s 出现 3s 假命中
  （橙灯误亮，且该 3s 内 `Esc` 变真实宿主 Esc、回不了 Normal）。窗口戳与比较改为
  `vim_timer_start32()` / `timer_elapsed32()`。

---

## 3. 状态机隐患修复（A1–A8）

| 编号 | 问题 | 目标做法 |
|---|---|---|
| A1 | 计数泄漏（`3x` 后 `j` 跳 3 行） | 计数**只作用于移动/缩进/行操作**；ctx 随命令严格重置 |
| A2 | `.` 回放卡在 operator-pending / 录制态 | 记录**命令 token**，模式为显式状态 |
| A3 | NKRO 下卡 motion 自动重复 | **不 `clear_keyboard()`**（不因进入模式丢按住键） |
| A4 | let-through 取消过宽（误取消） | 仅对**显式 pending** 清空/重新识别 |
| A5 | 可视文本对象取消卡状态 | 文本对象已**整体剔除** |
| A6 | 左右混合修饰符打包错 | **不再打包修饰键**（物理影子） |
| A7 | 计数上限溢出/看门狗 | 上限 **2 位（≤99）**，第 3 位起忽略 |
| A8 | 直接映射的模键码修饰位 | 不再打包；修饰位由物理影子提供，键码自带修饰位不被剥离 |

---

## 4. 有意保留的差异 / 取舍

- 操作符待定遇非法键**重新识别**（不吞键），使 `d` 后 `x` 表现为"取消 `d` + 删一个字符"。
- 编辑器无关：不区分编辑器，`emit` 用**单一固定映射**。
- 可接受的近似：`dd/yy/cc` 首/末/空行边界、`e≈w`、无冒号命令等，按硬件限制忽略。

---

## 5. 问题记录（V1.0 引擎/集成教训）

| 编号 | 问题 | 处理 |
|---|---|---|
| **E1** | `dd` 依赖编辑器 / 末行删不掉 | 定稿 `Home×2 + Shift+End + Ctrl+X + Backspace`：**末行可删**、缩进正确、`p` 可粘；代价：**首行留一个空行**、`dd` 是两次宿主编辑 |
| **E2** | 修饰键"幽灵"放大 → 卡 `Shift` | 旧实现打包 + 无条件 `set_mods`，一次释放被吞即反复重装；目标：**不打包、不回写**，只按物理修饰键影子动作 |
| **E3** | Alt+Tab 卡 `Tab`（吞 key-up） | 旧实现吞释放；目标：**key-up 一律透传**（held motion 例外） |
| **E4** | `dd` 撤销需要两步 | `dd` 是两次宿主编辑；**方案 A：取消自动双撤销**——一次 `u` 只恢复一半，需再按一次 |
| **E5** | 子模块版本错配 | bump 后**必须重编并校验** `output/`、`.build/` 哈希（产物 ≠ 源码） |
| **E6** | 其它集成 | 裁剪依赖、`layer_count`、rgbrec Fn 特判等 |

---

## 6. 构建与结构

- 目标为**与 QMK 解耦的核心层** `engine/`（见 [`design.md`](design.md) §4.6），
  纯 C，附 `engine/Makefile`，可 `make -C engine test` 跑主机单测。
- 接回固件（替换现 `process_func`）属后续阶段，通过 `SRC +=` 编入 keymap。
- 旧实现目录：`src/{vim,modes,actions,motions,numbered_actions,mac_mode,process_func}.c/.h`。

---

## 7. VISUAL_LINE 与真实 Vim 对齐（2026 重写）

**背景**：旧版把"整行近似"实现为"锚点固定在**被选首行行首** + 字符级 `Shift+↑/↓` 扩展"。
一旦活动端越过锚点（`k`/`b`/`gg`），字符选区立刻退化成"一个换行"，而动作前的
`Shift+Home`+`Shift+End` 又作用在活动端所在行，把范围进一步缩错：

| 缺陷 | 复现 | 旧行为（数据损坏） | 真实 Vim | 现行为 |
|---|---|---|---|---|
| P0 | `V k d`（光标在 L2） | 删掉换行 → **把 L1、L2 拼接** | 删掉 L1、L2 两整行 | `Shift+Up`,`Down`×2,`Home`,`Shift+Up`×2,`Ctrl+X` |
| P0 | `V gg y`（光标在 L3） | 剪贴板 `"\nL1\nL2\n"`（**丢首尾正文**） | `L1\nL2\nL3\n` | `Down`,`Home`,`Ctrl+Shift+Home`,`Ctrl+C` |
| P1 | `V s` | 只删 1 个字符 | 与 `V c` 等价（删整行 + 留空行 + Insert） | `Ctrl+X`,`Shift+Enter` |
| P1 | `V d` | 只清正文、留一个空行 | 删掉整行 | 同上 + `Shift+Right` 纳入换行 |
| P2 | `V y` | 字符级寄存器（无换行），`V y p` 变空操作 | linewise 寄存器 | DOWN 态补 `Shift+Right` |
| P2 | `V j`（L1=20、L2=21 字符） | 漏 L2 第 21 字符 | 两整行 | 先 `Shift+Down` 再 `Shift+End` |
| P2 | `V l l y` | `Shift+Right` 跨行 → 复制 2 行 | 行范围不变 | `l` **不发键** |
| P2 | `V y` 后敲键 | 残留选区被下一个键替换 | 取消选区 | `y`/`p` 后补发 `Esc` |
| P3 | `v` 后按 `V` | 吞键、不切换 | 切到行选 | `Home`,`Shift+End` 切行选 |

**设计**：引擎自记**行偏移** `off = 光标行 − 锚行 A`，用两种锚点表示重建整行字符选区
（`off≥0`：锚在 A 行首、活动端在 `A+off` 行尾；`off<0`：锚在 A+1 行首、活动端在 `A+off` 行首，
**天然含换行**）。重锚**只在方向翻转**时发生。动作因此与选区形状/方向解耦。
详见 [`design.md`](design.md) §4.9 与 [`readme.md`](readme.md) §7。

**已知偏差**（真实 Vim 与本实现的差距，均已在文档声明）：`w`/`e`/`b` 按"整行 ±1"近似
（Vim 是**词**动作）；动作后光标停在活动端（Vim 停在选区首行）；末行无换行时 `V d` 留一个空行；
`G`/`gg` 之后行偏移失效，跨锚点移动不再重锚；Normal 的 `p` 只发 `Ctrl+V`（不做"下一行新建粘贴"）。

### 7.1 真实 Vim 比对与独立审计（2026-09）

用 `/usr/bin/vim.tiny`（VIM 9.1）对 25 条必需矩阵 + 20 条扩展 + 17 条重锚算术逐条取真值，
再用宿主编辑器模型回放引擎**实际发出的键码**逐条比对。结论：

- **25/25 缓冲区文本与真实 Vim 完全一致**；`V k`/`V gg`/`V G` 与方向翻转重锚的 Up/Down
  次数与方向在 off ∈ {−5,−2,−1,+1,+2,+5} 上全部正确；行选 linewise 复制/删除/修改均生效。
- 寄存器 19/25 一致，差异全部落在已声明偏差（design §4.9 的 ①~⑩）。
- **审计发现一处旧清单未覆盖的数据损坏**：`abs`（`G`/`gg` 之后）状态下**反向越过锚行** ——
  `V G k k k y` 得到空串、`V gg j y` 得到 `\nL3\n`（Vim 为 `L2\nL3\n`），用 `d`/`x` 会删错范围。
  根因：`abs` 路径一律用 `Shift+End` 收边，而 `gg` 之后活动端是**上**边界。**已修**：按
  `s_vl_up` 选 `Shift+Home`/`Shift+End` —— 自然方向（`gg` 后向下、`G` 后向上）现与 Vim 一致
  （`V gg j y` = `L2\nL3\n`、`V gg j d` = 删 L2..L3）。反向越过锚行仍是固有限制（design §4.9 ④）。
- 另有三处差距原先未列入清单，已补进 design §4.9 的 ⑦⑧⑨：`3G`/`3gg` 计数被丢弃、
  可视 `p` 不更新宿主剪贴板、`V` 后按 `v` 时宿主选区仍是整行。
- 自查另发现并修复一处**容量型**缺陷（逻辑变异审计覆盖不到）：重锚是 O(|off|) 键码，
  `V 99 k` = 300 键 > `emit.c` 的 `EMIT_CAP=256` 且溢出**静默丢键** → 选区错乱。
  已把重锚改成从当前光标直接重建（≤ n+3）并加 `KV_VLINE_MAX_OFF=100` 跨度上限，
  单条命令 ≤102 键（与 Normal `99dd`=103 同量级）。详见本文件 §7 与 design §4.9。

### 7.2 `.` 重复的是「上一次修改」（2026-09 用户实测）

**缺陷**：旧实现把**裸移动**也当成一条"命令"提交到 repeat 录制，于是 `dw` → `w` → `.`
回放的是 `w` 而不是 `dw`（用户实测报告：「总是记录移动指令，不记录编辑」）。

**真实 Vim 实测**（vim.tiny 9.1，模板 `aaa|bbb|ccc|ddd`，统一用 `x` 先造一条修改）：

| 序列 | 结果 | `.` 实际重复的是 |
| :--- | :--- | :--- |
| `x` `w` `.` | `aa\|bb\|ccc\|ddd` | `x`（移动不夺走目标） |
| `w` `.`（无更早修改） | 不变 | 什么都不做 |
| `x` `yy` `w` `.` | `aa\|bb\|ccc\|ddd` | `x`（**复制不是修改**） |
| `x` `>>` `w` `.` | `\taa\|\tbbb\|…` | `>>` |
| `x` `p` `w` `.` | `aaa\|babb\|…` | `p` |
| `x` `J` `w` `.` | `aa bbb ccc\|ddd` | `J` |
| `x` `dd` `w` `.` | `bbb\|ddd` | `dd` |
| `x` `dw` `w` `.` | `\|\|ccc\|ddd` | `dw` |

**修复**：新增 `s_rec_change`，只有含"修改"的录制才 `rec_commit()`；命令回到 `Idle` 时若录制里
没有修改则**丢弃本次录制**（`s_last` 保留）。`y`/`Y` 与裸移动都不再夺走 `.` 的目标。
详见 [`design.md`](design.md) §4.7 与 [`readme.md`](readme.md) §3。

### 7.3 `cc`/`S` 留空行、`J` 插空格（2026-09 全面审核）

引擎语义审核（214 用例对照 vim.tiny 9.1）发现两处**真实缺陷**（均可实现，非固有限制）：

| 命令 | 旧行为 | 真实 Vim | 现行为 |
| :--- | :--- | :--- | :--- |
| `cc` / `S` / `Ncc` | 与 `dd` 相同（多发 `Backspace`），**整行被并掉**：`L1\|L2\|L3` ⇒ `L1\|L3` | **留一个空行**：`L1\|\|L3`；`2cc` ⇒ `L1\|\|L4` | 去掉 `Backspace`：`Home×2 → Shift+End [→ Shift+Down×(n−1)] → Ctrl+X`，三种行位置都与 Vim 一致 |
| `J` | `End → Delete`，得到 `threefour` | 插一个空格：`three four` | `End → Space → Delete`。已知偏差：Vim 还会去掉下一行前导空白，固件读不到空白长度 |

`dd` 的"首行留空行"是 §4.8 E1 里**已记录的历史取舍**（纯键注入下无法同时兼顾首行与末行），
本次不改，但在 `readme.md` 中与 `cc` 的区别写清楚了。
