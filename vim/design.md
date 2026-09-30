# vim 引擎：技术架构与实现细节

> 面向实现。描述 vim 引擎的**目标架构**（按模式解耦 + token 队列 + 表驱动 + pending 严格清空），
> 借鉴 Vim 源码的语法模型，并针对固件"只能发键序列、不能改缓冲区"的现实设计。
> 使用说明见 [`readme.md`](readme.md)；变更与历史问题见 [`changes.md`](changes.md)；
> 测试见 [`testcase.md`](testcase.md)。
>
> 注：本文件为重构设计的**唯一权威**，取代 `qmk-vim/docs/REDESIGN_PLAN.md`（历史草案，勿引）。
> 注：文中出现的键盘名（如 QK61/NUT65）仅为**参考示例**；本仓库共享层不含任何键盘专属实现或测试。
> 注：**Caps 长按模块**（键盘层模式：长按 Caps 的键映射）见 [`../caps/design.md`](../caps/design.md)。
> 注：**变更流程（文档先行）与共享层↔键盘分支的同步/验证规范**见 [`../qmk/README.md`](../qmk/README.md)；
> 该规范与本文档同级权威，改动顺序、子模块同步、验证清单、坑位清单均以其为准。
> 「文档先行」认可的文档文件集：`vim/{design,readme,changes,testcase}.md`、`caps/{design,readme,changes,testcase}.md`、
> `fn/{readme,changes}.md`、`qmk/README.md`。

---

## 1. 背景与目标

现实现（qmk-vim fork，V1.0）是一个**全局 `process_func` 函数指针状态机**：

- 在 `process_normal_mode` / `process_visual_mode` / `process_insert_mode` /
  `process_vim_action` / `process_g_cmd` 等之间切换，状态散落在全局变量里；
- 每个按键**打包修饰键**（`(kc & 0x1FFF) | mods<<8`）并 `clear_mods()` / `set_mods()`；
- 各模式处理函数在**非 press（key-up）也 `return false`**，会吞掉释放事件。

由此产生的问题（实案见 [`changes.md`](changes.md) E1–E6）：

- **幽灵修饰键**：一次释放被吞，修饰位就被每条命令反复重装 → 卡 `Shift`；
- **吞 key-up**：press 透传而 release 落入引擎 → 卡 `Tab`（Alt+Tab）；
- **计数泄漏**：全局 `motion_counter` 清理点分散 → `3x` 后 `j` 跳 3 行；
- **模式耦合**：`.` 回放、文本对象取消等易卡在半途状态。

**目标**：改为**按模式解耦 + token 队列 + 表驱动 + pending 严格清空**，从结构上根治上述问题，
并做到与 QMK 解耦、可主机侧单测。

---

## 2. Vim 源码参考

（以 Vim 官方源码 `src/` 为蓝本，取可复用的语法模型。）

### 2.1 输入层：typeahead 队列 + 单槽回退
- `typebuf_T`：字符缓冲 + 出队偏移；多档输入（宏展开 / typeahead / 物理键）。
- `vgetc()` 取键，`vpeekc()` 前瞻，`vungetc()` **单槽回退**（"多看一个再放回"）。
- 结论：**队列 + 前瞻 +（可选）单槽回退**即可支撑解析。

### 2.2 模式：`State` 位 + 每模式独立循环
- `State` 基础模式（NORMAL / INSERT / CMDLINE…）+ 各模式独立循环（`normal_cmd` / `edit`）。
- **瞬态模式是派生的**，不另存状态：Visual=`MODE_NORMAL && VIsual_active`；
  Operator-pending=`MODE_NORMAL && finish_op`。
- `main_loop` 持有持久 `oparg_T oa`，所以 `d` … `w` 跨按键的状态天然保留。

### 2.3 命令表 `nv_cmds[]`
```c
struct nv_cmd { int cmd_char; nv_func_T cmd_func; short_u cmd_flags; short cmd_arg; };
```
- `find_command()` 建索引查表；Normal/Visual **共用同一张表**（函数内按状态分支）。
- 加命令 = 加一行，不改解析器。

### 2.4 命令上下文 `cmdarg_T`
`prechar`(g) / `cmdchar` / `nchar`（第二/三键）/ `count0`/`count1` / `arg` / `retval`。
计数**跨操作符折叠**：`3dw` 内部当 `d3w`；前后都带则相乘。

### 2.5 操作符 `oparg_T` + 两遍式
`op_type` / `motion_type`（**只有 MCHAR / MLINE / MBLOCK**）/ `motion_force` / `inclusive` /
`start`/`end` / `line_count` / `block_mode`。

- 第一遍 `d` 只设 `op_type` 与起点；第二遍 motion/object 设区间并移动光标；
  `do_pending_operator` 收尾。
- **motion/object = 区间生产者；operator = 区间消费者**。
- 只有 `op_change` 例外：先删再进 Insert。

### 2.6 重放 `.`
`prep_redo` 把命令序列写进 redo buffer；`.` 把记录**塞回输入队列**逐字符重放。

### 2.7 Visual
`VIsual`/`VIsual_active`/`VIsual_mode`；进入 `n_start_visual_mode`；
**复用 Normal 命令表**（在函数内按 `VIsual_active` 分支）。

### 2.8 关键洞察
1. Normal 是"一次按键一次处理"，operator 状态存在持久上下文里跨调用延续；
2. 瞬态模式（Visual/op-pending）是**派生**的；
3. 表驱动 + payload：加命令不改解析器；
4. motion/object 是区间生产者，operator 是区间消费者；
5. 输入只需队列 + 前瞻；`.`/宏靠"塞回队列"重放。

> 与 Vim 的根本差异：Vim 直接改缓冲区；**固件只能发键序列**，因此本引擎的
> "operator 消费者"是 `emit(command) -> 宿主键序列`，而非 `op_delete()`。

---

## 3. 现 qmk-vim 实现与缺陷

（V1.0，代码 commit `62bb338`。）

### 3.1 入口 `process_vim_mode`
`vim_enabled` → Layer/Mod-Tap 解包 → 范围透传 → Insert 快路径 → **打包修饰键** →
`clear_mods()`/`clear_oneshot_mods()` → `process_func(keycode, record)` →
`set_mods(mods)`（仅透传时恢复 oneshot）→ 返回是否消费。

### 3.2 模式与操作符
- 模式入口 `normal_mode`/`insert_mode`/`visual_mode`/`visual_line_mode` 切换全局 `process_func`。
- `insert_mode()` **无条件 `clear_keyboard()`**。
- 操作符：`start_*_action` 设 `action_key`/`action_func` 并切到 `process_vim_action`。
- **行缩进 `N>>`/`N<<` = N 行**（2026-09 修正）：`n=1` 发 `Home, Tab/Shift+Tab`（**无选区**＝在行首
  插入/删除一个 Tab）；`n>1` 发 `Home, Home, Shift+Down×n, Tab/Shift+Tab, Esc, Up×n, Home`（`>` 再补
  `Right`）。真实 Vim 的 `>>` 缩进 1 行、`2>>` 2 行、`3>>` 3 行；旧实现用 `Shift+Down×(n-1)`，宿主侧
  半开选区只覆盖 **n-1** 行，故 `2>>`/`3>>` 都少缩进一行。
  `Esc` 取消宿主**残留选区**——宿主在 `Tab` 缩进后保留高亮选区，不发 `Esc` 时紧随的 `x`/`p`/`.` 会
  替换整段选区（**数据损坏**，独立审查 D6）；`Up×n`/`Home` 把光标拉回**范围内首行**（Vim 的 `>`/`<`
  把光标留在范围内首行的**首个非空白**，`>>x` 删的是缩进后的首字符）。`n=1` 的 `Home, Tab` 不建选区，
  插入的 Tab 正好把光标顶到"首个非空白"处，与 Vim 完全一致。
- **缩进恒为整行操作**（2026-09 修正）：真实 Vim 的 `>`/`<` 与 `h`/`l`/`0`/`^`/`$` 组合也是**整行**
  （`>h`/`>l`/`>0`/`>^`/`>$` 都只缩进**当前行**），且 `h` 在**列 0 不跨行**——旧实现按字符级发
  `Shift+Left×n`，宿主 `Shift+Left` 在列 0 会**回绕到上一行行尾**、把上一行缩进（**数据损坏**，
  独立审查 D4）。现改为按行处理。`N>$` 是例外：`$` 带计数会下移 N−1 行，故 `2>$` = 2 行（与 `2>>` 同）；
  `N>0`/`N>^`/`N>h`/`N>l` 的计数不改变行范围（仍 1 行）。
- **行选动作（`j`/`k`/`G`/`gg`）+ 操作符 = 整行操作**（2026-09 修正）：真实 Vim 的
  `dj`/`dk`/`dG`/`dgg`/`cj`/`yG`… 是**行选**（`dj` 删当前行+下一行 = 2 行，`2dj` = 3 行）。
  发射分方向（半开区间 `[anchor,cursor)` 决定，2026-09 修正）：
  - **向下** `j`/`G`：`Home → Shift+Down×(n+1) → 动作`；`G` 用 `Home → Ctrl+Shift+End → 动作`。
  - **向上** `k`：`Home → End → Right → Shift+Up×(n+1) → 动作`。`End, Right` 把光标**越过当前行
    的行尾换行**到下一行行首，锚点落在那里；半开区间才**包含当前行的换行**（否则 `dk` 只删掉
    行内容、留下一个空行）。末行（无换行）时 `Right` 无效、锚点落在缓冲末尾，同样正确。
  - **向上到顶** `gg`：`End → Right → Ctrl+Shift+Home → 动作`（同理，锚点越过当前行换行）。
  - **`c` + 行选动作**（`cj`/`ck`/`cG`/`cgg`/`Ncj`）在 `Ctrl+X` 之后补 **`Shift+Enter, ←`**：
    真实 Vim 的 `c`+行选移动与 `cc` 一样**留一个空行**（`L1|L2|L3|L4` 上 `cj` ⇒ `L1||L4`）。
    补的 `Shift+Enter` 造出那个空行，但它会把宿主光标顶到**接替行行首**，而 Vim 把光标留在
    空行**上**——差一行是**缓冲区可见的**（`cjx` 会删掉接替行的首字符 = 数据损坏），故再补 `←`
    退回空行（2026-09 修正，独立矩阵测试 "D5-residual" 组）。
  旧实现从**当前列**开始扩选，会删掉"上一行尾部 + 下一行头部"（数据损坏）。
- `dd`（2026-09 修正）：`Home×2 → Shift+End → **Shift+→** → Shift+Down×(n−1) → Ctrl+X`
  （n=1 时无 `Shift+Down`）。`Shift+→` 把**行尾换行**纳入宿主半开选区：`Shift+End` 只到末字符
  之前，删完在**首行**会留下一个空行（`dd`@L1 得 `|L2|L3`，Vim 得 `L2|L3`，数据损坏）。
  `Ctrl+X` 之后宿主光标正好停在**接替行行首**，与 Vim 一致，故旧版末尾的
  `Backspace, Down, Home` 全部删除（旧版靠 `Backspace` 并掉换行、再 `Down,Home` 挪光标）。
- `cc` / `S` / `Ncc`（2026-09 修正）：`Home×2 → Shift+End → **Shift+→** [→ Shift+Down×(n−1)]
  → Ctrl+X → **Shift+Enter → ←**（**不发 Backspace**）。真实 Vim 的 `cc`/`S` 会**留一个空行**
  （`L1|L2|L3` 上 `cc` ⇒ `L1||L3`，`2cc` ⇒ `L1||L4`），同时无名寄存器里是**行级**内容
  （含行尾换行）。旧实现**只删行内容、不含换行**，于是寄存器变成字符级，
  随后的 `p` 会当成字符级往**行内**粘（`cc<Esc>p` 模型 `L1\n\nL2L3\nL4`、Vim `L1\n\nL2\nL3\nL4`）。
  现先把换行纳入选区（寄存器行级），再补 `Shift+Enter` 造出那个空行、`←` 把
  光标退回空行：`cc`/`2cc`/`S`/`2S`/`ccp`/`Sp`/`2ccp` 在首/中/末行的
  **缓冲区与寄存器**均与 Vim 一致（前/后探针 24 用例：21 修好、0 回归）。
  残留：`cc<Esc>p` 仅光标位置差异（行级粘贴的光标偏差，已在 §4.4 声明）。
- `J`（2026-09 修正）：`End → Space → Delete`。真实 Vim 的连接会**插一个空格**
  （`three` + `four` ⇒ `three four`）；旧实现只发 `End → Delete`，得到 `threefour`。
  **已知偏差**：Vim 还会去掉下一行的**前导空白**，固件读不到空白长度，故保留。
- 计数：全局 `motion_counter`（名义 ≤2 位，但存在越界路径），`DO_NUMBERED_ACTION` 循环执行。

### 3.3 缺陷（对应 E1–E6 / A1–A8）
| 编号 | 问题 | 根因 |
|---|---|---|
| E1 | `dd` 依赖编辑器 / 末行 | 纯键注入取舍 |
| E2 | 幽灵修饰键卡 `Shift` | 打包 + 无条件 `set_mods` |
| E3 | Alt+Tab 卡 `Tab` | 吞 key-up |
| E4 | `dd` 撤销要两步 | 两次主机编辑 |
| E5 | 子模块错配 | 产物 ≠ 源码 |
| E6 | 集成（层数、rgbrec 等） | 键盘侧 |
| A1 | 计数泄漏 | 全局计数清理分散 |
| A2 | `.` 卡 pending | 记录/模式耦合 |
| A3 | NKRO 卡 motion | `clear_keyboard` + held 判断失效 |
| A4 | let-through 取消过宽 | `process_func != normal` 误判 |
| A5 | 可视文本对象取消 | 状态耦合 |
| A6 | 左右混合修饰符打包错 | 位移打包 |
| A7/A8 | 计数上限、模键码位 | 边角 |

---

## 4. 新引擎设计

### 4.1 已定稿决策

| # | 决策项 | 结论 |
|---|---|---|
| 1 | 多键挂起打断 | **严格清空**：遇到非期望键，**立即清空 pending**，再**重新识别**该键——是 vim 键码则当作新命令首键；否则**原样透传宿主** |
| 1b | 兜底超时 | **无超时** |
| 2 | 计数 | 仅前缀；操作符前后**相乘**（`2d3w`=`d6w`）；**最多 2 位（≤99），第 3 位起忽略**（`123w`≡`12w`）；首位非 `0`；无计数时 `n=1` |
| 2b | 计数作用域 | 移动、缩进、行操作，**以及单键编辑**（`Nx`/`NX`/`Ns`/`NJ`/`Np`/`NP`/`Nu`/`N.`/`NC`/`ND`/`NY`，2026-09 起）；仍丢弃：`G`/`gg`/`ZZ`/插入键/`v`/`V`。`G`/`gg` 虽属移动，**任何上下文都丢弃计数**（`2dG`≡`dG`、`2dgg`≡`dgg`）；操作符/缩进后的 `0` 亦丢弃 n（`2d0`≡`d0`） |
| 3 | 模式范围 | NORMAL + INSERT + VISUAL/Visual-Line（含瞬态 OP_PENDING） |
| 4 | 编辑器适配 | 与编辑器无关；**无 profile 层**；`emit` 单一固定映射 |
| 5 | 落地方式 | 新核心层 `engine/`（与 QMK 解耦）+ 主机单测；接回固件属后续阶段 |
| 6 | 硬件/系统 | 电源/bootloader/USB/无线/RGB/编码器不在引擎范围 |
| 7 | emit 方式 | **非阻塞队列**：命令键序列入队，由 `kv_task()`（housekeeping）按计时发送；**禁用 `wait_ms`** |
| 8 | key-up | **一律透传**；**唯一例外**：按住连发移动 `h/j/k/l`（down 注册宿主方向键 / up 反注册） |
| 9 | 严格清空 | 重新识别用**循环**处理，不递归、不重复 emit |
| 10 | dd 撤销 | **方案 A**：保留两动作 `dd`，**移除自动双撤销**；一次 `u` 只恢复一半；末行仍可删 |

### 4.2 核心模型：单键 vs 多键
- **单键指令**：入队 → peek → pop → **立即 emit**。
- **多键指令**：pop 后**不再取后续键**，挂起信息存 `ctx`（count/op/前缀/nchar），
  待"结束键"到达后一次性 emit。
  - 操作符 `d/y/c` 的结束键 = 移动；前缀 `g`/`Z` 的结束键 = 第二键；计数的结束键 = 后续命令。
  - 吸收的键不发往宿主；取消时因从未 emit，宿主零副作用。
- **严格清空**：非期望键 → 清空 pending → 重新识别（vim 键码当首键；否则透传）。
- **Esc**：立即取消 pending（回 NORMAL，什么都不发）。
- **悬空计数**：`dw3` = `dw` 执行后，`3` 进入 `Cnt`，**无超时**等待下一键决定（`3w` 套用 / `3x` 丢弃计数 / Esc 取消）。

### 4.3 Normal 指令集（冻结）

**单键**：`h j k l` `w W b B e E` `0 ^ $` `G` `i I a A o O` `C D Y X` `x s` `p P` `J` `u` `.` `v` `V` `S`(≡`cc`，可带计数)

**多键**：计数 `[1-9][0-9]?`（元变量；**字面键 `N` 不作计数**，按普通键透传）、`d y c`(操作符) `< >`(缩进) `g`(→`gg`) `Z`(→`ZZ`)

> `v`/`V` 为 Visual / Visual-Line 入口（模式切换键，非命令）。

```regex
N   = [1-9][0-9]?   # 最多 2 位（<=99）；第 3 位起忽略；首位非 0
# 0 只在"无计数"时作动作（计数里的 0 归 [0-9]）：d20w = 计数20、d0 = 删到行首
MOT = (?:[hjkl]|[wWbBeE]|[$^]|0|G|gg)   # 不含计数；计数由外层 N? 拼接

(?: N?MOT               # 独立运动（含计数；G/gg 丢计数）
  | N?[dcy]MOT          # 操作符 + 移动
  | N?(?:dd|cc|yy)      # 行操作（自叠）
  | N?S                 # S = cc 别名（接受计数）
  | N?[<>]MOT           # 缩进 + 移动
  | N?(?:>>|<<)         # 行缩进（自叠）
  | [CDYXxs]
  | [iIaAoO]
  | [pP]
  | [vV]
  | J
  | u
  | \.
  | ZZ
)
```
> 行操作/行缩进用显式 `dd|cc|yy`、`>>|<<` 表达，避免反向引用歧义。
> `G`/`gg` **在任何上下文均丢弃计数**（见 §4.4）。
> 正则为**简化描述**：`N` 的"第 3 位起忽略"、**`0` 的计数续接**（`20`/`d20`：第 2 位 `0` 续接计数、非"行首"命令）、`G`/`gg` 的丢计数、**计数 + 丢弃计数键**（`x s X C D Y p P i I a A o O v V J u . ZZ`）、以及操作符/缩进后置计数配 `G`/`gg`（`d2G`/`d2gg`/`>2G`/`>2gg`）均由解析层/状态机处理。

### 4.4 多键状态机（严格清空）

**状态**

| 状态 | 含义 | 期望的下个字符 |
|---|---|---|
| `Idle` | NORMAL 空闲 | — |
| `Cnt` | 已收计数 | 数字 / 操作符 / 缩进 / 移动 / `g` / `G` / `S` / `Z`（其它 → 丢弃计数后重新识别） |
| `Op` | 已收操作符 `d/y/c` | `[1-9]` / `0` / 移动 / `G` / 同字符 / `g` |
| `OpCnt` | 操作符 + 计数 | 数字 / 移动 / `G` / `g` |
| `Ang` | 已收 `<`/`>` | `[1-9]` / `0` / 移动 / `G` / 同字符 / `g` |
| `AngCnt` | 缩进 + 计数 | 数字 / 移动 / `G` / `g` |
| `Gp` | 已收 `g` | `g` |
| `Zp` | 已收 `Z` | `Z` |

**转移**（`Esc` 处处取消；非期望键 → 清空 + 重新识别，见 4.5）

| 当前 | 事件 | 下一状态 | 动作 |
|---|---|---|---|
| `Idle` | `[1-9]` | `Cnt` | n=digit |
| `Idle` | `d/y/c` | `Op` | op=char |
| `Idle` | `S` | `Idle` | **emit**(cc)（`S`≡`cc`，单键） |
| `Idle` | `<`/`>` | `Ang` | ang=char |
| `Idle` | `g` | `Gp` | — |
| `Idle` | `Z` | `Zp` | — |
| `Idle` | 移动 / 单键命令 | `Idle` | **emit** |
| `Idle` | `i/I/a/A/o/O` | `INSERT` | 进入插入 |
| `Idle` | `v` / `V` | `VISUAL` / `VISUAL_LINE` | 进入可视 |
| `Cnt` | `[0-9]` | `Cnt` | n=n*10+d（最多 2 位；第 3 位起忽略） |
| `Cnt` | `d/y/c` | `Op` | 携带 n |
| `Cnt` | `<`/`>` | `Ang` | 携带 n |
| `Cnt` | 移动（不含 `G`/`gg`/`0`） | `Idle` | **emit**(移动×n) |
| `Cnt` | `S` | `Idle` | **emit**(cc×n)（`S`≡`cc`，接受计数） |
| `Cnt` | `g` | `Gp` | 丢弃 n |
| `Cnt` | `Z` | `Zp` | 丢弃 n（供 `3ZZ`≡`ZZ`） |
| `Cnt` | `G` | `Idle` | **emit**(G)（丢弃 n） |
| `Cnt` | 不接受计数的键 | `Idle` | 丢弃 n 后**交回解析器重新识别** |
| `Op` | 同字符 `dd/yy/cc` | `Idle` | **emit**(行操作×n) |
| `Op` | 移动（不含 `G`/`gg`/`0`） | `Idle` | **emit**(op+移动×n) |
| `Op` | `G` | `Idle` | **emit**(op+`G`)（丢弃 n，`2dG`≡`dG`） |
| `Op` | `[1-9]` | `OpCnt` | n2=digit |
| `Op` | `0` | `Idle` | **emit**(op+`0`)（丢弃 n） |
| `Op` | `g` | `Gp`(ctx=operator) | 丢弃 n（供 `dgg`/`d2gg`） |
| `OpCnt` | `[0-9]` | `OpCnt` | n2=n2*10+d（最多 2 位） |
| `OpCnt` | 移动（不含 `G`/`gg`/`0`） | `Idle` | **emit**(op+移动×(n*n2)) |
| `OpCnt` | `G` | `Idle` | **emit**(op+`G`)（丢弃计数，`d2G`≡`dG`） |
| `OpCnt` | `g` | `Gp`(ctx=operator) | 丢弃计数（`d2gg`≡`dgg`） |
| `Ang` | 同字符 `>> <<` | `Idle` | **emit**(行缩进×n) |
| `Ang` | 移动（不含 `G`/`gg`/`0`） | `Idle` | **emit**(缩进+移动×n) |
| `Ang` | `G` | `Idle` | **emit**(缩进+`G`)（丢弃 n，`2>G`≡`>G`） |
| `Ang` | `g` | `Gp`(ctx=indent) | 丢弃 n（保留缩进） |
| `Ang` | `0` | `Idle` | **emit**(缩进+`0`)（丢弃 n） |
| `Ang` | `[1-9]` | `AngCnt` | n2=digit |
| `AngCnt` | `[0-9]` | `AngCnt` | n2=n2*10+d（最多 2 位） |
| `AngCnt` | 移动（不含 `G`/`gg`/`0`） | `Idle` | **emit**(缩进+移动×(n*n2)) |
| `AngCnt` | `G` | `Idle` | **emit**(缩进+`G`)（丢弃计数） |
| `AngCnt` | `g` | `Gp`(ctx=indent) | 丢弃计数 |
| `Gp` | `g` | `Idle` | ctx=op→**emit**(op+gg)；ctx=indent→**emit**(缩进+gg)；否则 **emit**(gg) |
| `Zp` | `Z` | `Idle` | **emit**(ZZ) |

```
            非期望键: 清空 pending -> 重新识别
             ├─ vim 键码 -> 当新命令首键
             └─ 非 vim 键码 -> 原样透传宿主
        ┌────────────────────────────────────────────────────────────┐
        ▼                                                            │
   ┌─────────┐ d/y/c  ┌─────────┐ 移动    ┌──────────────────────────┴┐
   │  Idle   ├───────►│   Op    ├────────►│ emit(op+移动×n)           │
   └─┬─┬─┬─┬─┘        └──┬──────┘ 同字符  └────────────┬───────────────┘
     │ │ │ │             │ [1-9]        └────────────► emit(行操作×n)
     │ │ │ │             ▼
     │ │ │ │          ┌─────────┐ 移动
     │ │ │ │          │ OpCnt   ├──────────────► emit(op+移动×(n*n2))
     │ │ │ │          └─────────┘
     │ │ │ │ g
     │ │ │ └──────────────────► ┌─────────┐ g
     │ │ │                      │   Gp    ├──► emit(gg / op+gg / 缩进+gg)
     │ │ │                      └─────────┘
     │ │ │ Z                    ┌─────────┐ Z
     │ │ └────────────────────► │   Zp    ├──► emit(ZZ)
     │ │                        └─────────┘
     │ │ < / >  ┌─────────┐ 移动
     │ └───────►│   Ang   ├──────────────► emit(缩进+移动×n)
     │          │         ├── 同字符 ────► emit(行缩进×n)
     │          └──┬───┬──┘
     │             │ [1-9]
     │             ▼
     │          ┌─────────┐ 移动
     │          │ AngCnt  ├──────────────► emit(缩进+移动×(n*n2))
     │          └─────────┘
     │ [1-9]  ┌─────────┐ 移动/缩进/行操作
     └───────►│  Cnt    ├──────────────► emit(…×n)
              └─────────┘
```

> 上图为转移表的直观视图（简略）；**以转移表为准**。所有 pending 态遇非期望键均"清空 + 重新识别"。

> **无超时**：所有 pending 态无限期等待下一个键码，不会自行清空。
> 计数上限 **2 位（≤99）**，第 3 位起忽略；无计数时 `n=1`。
> **计数态中 `0` 归 `[0-9]`**（续接计数），故移动行排除 `0`（单按 `0` 才是行首）。
> `OpCnt` / `AngCnt` **不接受同字符**（如 `d2d` 视为非期望 → 清空 + 重新识别；与 Vim 的 `d2d`≡`2dd` 不同，为**有意取舍**）。
> `Cnt` 态的 `G`/`gg` 属**显式例外**（转移表：`Cnt|G`→emit(G)、`Cnt|g`→`Gp`），不进入"不接受计数的键 → 重新识别"路径。
> `G`/`gg` **在任何上下文均丢弃计数**（`2dG`≡`dG`、`d2gg`≡`dgg`、`>2gg`≡`>gg`）。
> `S`≡`cc` **接受计数**（`3S`≡`3cc`）；`C/D/Y` 不接受计数（`S` 不在 `[CDYXxs]` 字符类中，单独由 `N?S` 表示）。
> 单键命令 `x s X C D Y S p P J u .` 见 §4.3（`S` 为单键、可带计数，见 `Cnt|S`）；本表只列多键转移。

### 4.5 严格清空（非期望键处理）
```
pending 态收到非期望键：
    1. 清空 ctx（丢弃已吸收的 d / 计数 / 前缀）
    2. 把该键交回解析器：
         - 属于 vim 键码集  -> 当作新命令首键重新处理
         - 不属于 vim 键码集 -> 原样透传宿主
```
- 不需要 `unget`（只需 peek 前瞻）；pending 不滞留，行为确定。
- 例：`d` 后按 `x` → 清空 `d`，`x` 作为单键命令执行。
- 例：`d` 后按 `F5` → 清空 `d`，`F5` 原样发宿主。

### 4.6 架构与文件清单
```
qmk-vim-fn/
  engine/                    # 与 QMK 解耦的纯 C 核心（可主机单测）
    include/kv.h             // 公共 API：kv_init / kv_kbd(kv_result_t) / kv_set_emit / kv_task + 查询/设置（见 §4.7）
    include/kv_kc.h          // kv_keycode_t 与修饰位（镜像 QMK 16-bit 帽子位布局，便于接回）
    src/classify.{h,c}       // keycode -> token 类别（含计数态的数字归类）
    src/ctx.{h,c}            // kv_ctx：count / op / 前缀累积与重置；状态枚举 kv_state_t
    src/emit.{h,c}           // 非阻塞发送队列（按计时排空，替代 wait_ms）
    src/command.{h,c}        // 命令/区间 -> 固定宿主键序列（与编辑器无关）
    src/engine.c             // feed() 解析循环（多键状态机见 §4.4）；严格清空；repeat 记录/回放；模式调度；kv_task()
    test/                    // 主机单测：喂 token -> 捕获 emit -> 断言（kvtest 记录器）
    Makefile                 // 仅主机测试；不参与 QMK 构建
  qmk/                       # QMK 适配层（依赖 QMK API；见 §4.12）
    vim_glue.{h,c}           // 引擎适配：物理修饰键影子(pipeline 第0步更新)、统一 press/release
                             // 配对表、held motion、Shift 折叠/CAG 透传、emit->register_code、极性封装
    vim_keymap_common.{h,c}  // 共享 keymap 层：vim_pipeline_process 单源拦截链、鼠标模式状态机、
                             // Caps tap/hold、Shift+Esc、§2.1 快捷键表、myfn 骨架、
                             // vim_task、vim_rgb_state_color 七色计算
  vim/  fn/                  # 本设计文档与 myfn 约定
```
> 说明：多键状态机（§4.4 的转移表）实现为 `engine.c` 中的显式转移函数（状态 × token 的 `switch`），
> 与转移表一一对应；命令→键序列映射集中在 `command.c`，便于逐条对照 §4.8。
> 术语统一：**engine**（本目录纯 C 核心）／**glue**（`qmk/` 下的 QMK 适配层）／**键盘层**（各 keymap：
> RGB、vendor 组合键、myfn 拦截、底排键位、§2.1 快捷键）。

### 4.7 关键接口
```c
/* 复位全部状态：模式=INSERT、vim 关、pending/repeat/emit 队列清空 */
void kv_init(void);

/* 解析器入口：只喂 key-down（basic keycode + 可选 Shift 帽子位，如 KV_C_G）。
 * key-up 不进引擎，由 glue 处理（§4.12）。
 * 返回 CONSUMED（引擎已处理/吞键，glue 须吞掉对应 release）或
 * PASSTHROUGH（非 vim 键，由调用方自行发给宿主，release 亦由宿主处理）。 */
typedef enum { KV_CONSUMED = 0, KV_PASSTHROUGH } kv_result_t;
kv_result_t kv_kbd(kv_keycode_t kc);

/* 输出回调：引擎把宿主键序列交给它；单测里换成记录器 */
typedef void (*kv_emit_fn)(kv_keycode_t kc);
void kv_set_emit(kv_emit_fn fn);

/* 由 housekeeping 调用：按计时发送 emit 队列（替代阻塞的 wait_ms） */
void kv_task(uint32_t now_ms);

/* ---- 查询/设置接口（供键盘层：Caps 恢复、RGB 指示、vim 开关、前置分支取消）---- */
kv_mode_t kv_get_mode(void);        /* 当前模式（含 Visual/Visual-Line/Mouse） */
bool      kv_vim_enabled(void);     /* vim 总开关 */
bool      kv_pending(void);         /* 是否有 pending（计数/操作符/前缀/缩进） */
void      kv_set_mode(kv_mode_t m); /* 直接设模式（如 Caps 恢复进入前模式） */
void      kv_enable(void);          /* 开 vim：固定从 INSERT 起 */
void      kv_disable(void);         /* 关 vim（RGB 红），见下方语义 */
void      kv_cancel(void);          /* 取消当前 pending（不发键），供键盘层前置分支 */
```

**模式/使能切换的清空语义（总规则）**：
- **任何模式切换一律丢弃 pending 状态机**——`kv_set_mode()`、`kv_enable()`、`kv_disable()`、
  `kv_cancel()`、进出 MOUSE 模式（由键盘层经 `kv_set_mode(MOUSE)` 实现），全部等价于先执行
  内部 `abort_input()`（清 ctx + 状态回 `Idle` + 清 repeat 录制缓存 s_rec）。
- **repeat 的 `s_last` 跨模式保留**（`.` 应能回放上一条编辑命令，如 `dd` 后 Caps 去 Insert 再回
  Normal 按 `.` 仍回放 `dd`）；被丢弃的半途命令不得混入录制（例：`2d` 后切换模式，`s_last`
  不得残留 `2d`）。
- **只有「修改缓冲区」的命令才提交录制**（真实 Vim 的 `.` 重复的是上一次**修改**，不是上一条命令）：
  - **提交**（成为 `.` 的目标）：操作符 `d`/`c` + 动作、`x X s C D p P J S`、缩进 `>> << >m <m`。
  - **不提交**：**裸移动**（`h j k l w b e W B E 0 ^ $ gg G` 及带计数的移动）、**纯复制** `y`/`Y`、
    仅计数、未完成的 `g`/`Z` 前缀、插入入口 `i I a A o O`。
  - 命令回到 `Idle` 时若录制里**没有**修改，则**丢弃本次录制**（`s_last` 保持不变），
    因此 `dw` → `w` → `.` 仍回放 `dw`（用户实测缺陷：旧实现回放了 `w`）、
    `x` → `yy` → `.` 仍回放 `x`（复制不夺走 `.` 的目标）。
  - **`N.` 重复 N 次**（真实 Vim：`dw` 后 `3.` 连删 3 个词）：计数作用于 `.`，
    按录制长度封顶 —— 总键码数 ≤99（`maxrep = 99 / 录制长度`），因为发送队列只有 256 格
    且溢出**静默丢键**。无 `s_last` 时 `N.` 无输出。
  - **已知限制**：插入（`i a o …`）键入的文本固件无法记录，故 `.` **不能**重复插入内容，
    只会回放更早的那条修改；`. ` 在 Visual 内为非法键（吞键留在 Visual）—— 真实 Vim 会把
    上次修改作用到选区，但固件读不到宿主真实选区范围，无法可靠实现。
- **`kv_disable()` 语义**：清 pending + **冲掉**未发送的 emit 队列 + `kv_kbd()` 一律返回
  `KV_PASSTHROUGH`；已由 held motion `register` 的宿主方向键由 glue 负责反注册（§4.12）。
- **`kv_enable()` 语义**：固定从 `INSERT` 模式开始（防回到 disable 前的 MOUSE/VISUAL）。
- **`kv_set_mode(KV_MODE_MOUSE)`**：MOUSE 属键盘层模式，引擎对 MOUSE 及之后新增的键盘层模式
  **一律返回 `KV_PASSTHROUGH`**，不做任何解析。

解析循环（表驱动，取代 `process_func`）：
```c
while (queue_has()) {
    kv_ctx next = ctx;
    token_t t = classify(peek());
    const kv_rule_t *r = table_for(mode, state)[t];
    if (!r) { /* 严格清空 + 重新识别（循环，不递归） */ }
    result_t res = r->handler(&ctx, &next, pop());
    ctx = next;
    if (res == NEED_MORE) continue;          // 多键待发
    if (res == COMMAND_DONE) { emit_command(&ctx, r->arg); repeat_record(&ctx); ctx_reset(&ctx); }
}
```

### 4.8 emit 固定映射（与编辑器无关）
| 命令 | 宿主键序列 |
|---|---|
| `h j k l` | ← ↓ ↑ → |
| `w W` / `e E` | Ctrl+→ |
| `b B` | Ctrl+← |
| `0` / `^` / `$` | Home / Home / End |
| `G` / `gg` | Ctrl+End / Ctrl+Home |
| `Nx`/`NX`/`Ns` | **一次**选中 N 个字符（`Shift+→×N` / `Shift+←×N`）再剪切 —— 寄存器拿到**全部 N 个**（逐个删只剩最后一个），键码 N+1 而不是 3N（`99X` = 297 键会撑爆 256 格队列 = **静默丢键**，独立审查 P0-1/P0-6） |
| `x` / `X` | Shift+→, Ctrl+X / Shift+←, Ctrl+C, Backspace（**写宿主剪贴板**，D7：`xp` 才能交换字符） |
| `s` | Shift+→, Ctrl+X, Insert（真实 Vim：`s` ≡ `cl`） |
| `C D Y` | `c$` / `d$` / `y$`（`Y` **≡ `yy` 行级**） |
| `S` / `NS` | 同 `cc` / `Ncc`（**×n 行**）；`cc`/`S` 的无名寄存器为**行级**（含行尾换行） |
| `dd` / `Ndd` | Home, Home, Shift+End, **Shift+→**, Shift+Down×(n-1), Ctrl+X（**×n 行**；n=1 时无 `Shift+Down`。`Shift+→` 把**行尾换行**纳入选区，否则**首行**会留下空行 = 数据损坏） |
| `Y` | **≡ `yy`（行级）**：Home, Home, Shift+Down×1, Ctrl+C, Esc, Up×1（真实 Vim 的 `Y` 是行级，不是 `y$`） |
| `NJ` | `End, Space, Delete, ←` ×(N−1)（N=1 时 1 次；真实 Vim：`J`/`2J` 连 2 行、`3J` 连 3 行） |
| `yy` / `Nyy` | Home, Home, Shift+Down×n, Ctrl+C, **Esc, Up×n**（**×n 行**；Esc 取消宿主残留选区，Up×n 把光标拉回原行——Vim 的 `y` 不移动光标） |
| `cc` / `Ncc` | Home, Home, Shift+End, Shift+Down×(n-1), change (+Insert)（**×n 行**；n=1 时无 `Shift+Down`） |
| `dw` / `d$` / `d0` | 选词/选到行首尾 → Ctrl+X |
| `p` / `P` | **按无名寄存器类型定位**（D7）：字符级 `p` = `→, Ctrl+V`（粘到光标字符**之后**）、`P` = `Ctrl+V`；行级 `p` = `End, →, Ctrl+V`（**下一行**新建一行）、`P` = `Ctrl+V`。寄存器类型由最近一次写剪贴板的命令跟踪（`dd`/`yy`/`dj`/`yG`/行选动作 = 行级；`x`/`X`/`s`/`dw`/`yl`/… = 字符级）；`Np`/`NP` **只定位一次**再 `Ctrl+V×N`（逐个定位+粘贴会把副本交错插入，独立审查 P0-5） |
| `J` | End, Space, Delete, ←（**插一个空格**，并把光标留在那个空格上——Vim 同；旧文档写 `End, Delete` 是错的） |
| `u` | Ctrl+Z（单次） |
| `ZZ` | Ctrl+S |
| `i I a A o O` | 见下 |
| `> <` | 缩进 / 反缩进，**恒为整行**：`n=1` → `Home, Tab/Shift+Tab`；`n>1` → `Home, Home, Shift+Down×n, Tab/Shift+Tab, Esc, Up×n, Home[, Right]` |
| `>h` `>l` `>0` `>^` | 同 `>>`（**当前行**，计数不改变行范围；`h` 在列 0 不跨行） |
| `>$` | `n=1` 同 `>>`；`n>1` 同 `N>>`（`2>$` = 2 行） |
| `>j` `>k` | `n+1` 行；`>j` 同 `(n+1)>>`，`>k` 用向上选区 `Home, End, Right, Shift+Up×(n+1), Tab/Shift+Tab, Esc`（光标已在范围内首行） |
| `>gg` | `End, Right, Ctrl+Shift+Home, Tab/Shift+Tab, Esc`（光标落在第 1 行首个非空白） |
| `>G` | `Home, Ctrl+Shift+End, Tab/Shift+Tab, Esc`（**光标停在文末**，见下偏差） |
| `>w` `>e` `>b` `>W` `>E` `>B` | 字符级扩选 → `Tab/Shift+Tab, Esc`（**行范围**由宿主半开选区决定，与 Vim 一致；**光标停在移动目标**，见下偏差） |

- 插入：`i` 原地；`I`=Home 后；`a`=→ 后；`A`=End 后；`o`=End,**Shift+Enter**；`O`=Home,**Shift+Enter**,↑。
- 粘贴定位（D7，2026-09 修正）：**按无名寄存器类型**选择定位键码，`kv_emit_paste(bool before)`
  内部读取 `s_reg_linewise`（由所有写剪贴板的 emitter 维护，`kv_init` 复位）：
  - **字符级**（`x`/`X`/`s`/`dw`/`yl`/`yw`…）：Vim 的 `p` 插在**光标字符之后** ⇒ `→, Ctrl+V`；
    `P` 插在光标字符之前 ⇒ `Ctrl+V`。
  - **行级**（`dd`/`yy`/`Y`/`dj`/`dk`/`dG`/`dgg`/行选动作…）：Vim 的 `p` 在**下一行**新建一行
    ⇒ `End, →, Ctrl+V`（`End,→` 越过行尾换行到下一行行首；末行无换行时 `→` 夹取到缓冲末尾，
    正好追加一行）；`P` 在**上一行** ⇒ `Ctrl+V`。
  旧实现一律 `Ctrl+V`（`P` 先 `←`），且 `x`/`X`/`s` 只发 `Delete`/`Backspace` **不写剪贴板**，
  于是 `xp` 交换字符、`ylp`、`ddp` 全都作用错位置（`yyp` 恰好蒙对）——独立审查 D7。
  **不跟踪 `u`**：真实 Vim 的 `u` 会恢复无名寄存器，本层不保存历史，属已知偏差。
- **`y` + 动作后的光标（D11，2026-09 修正前提）**：独立审查用 `vim.tiny`
  逐条实测发现，真实 Vim 把光标留在**被复制区间的起点**（`yh`→列−1、`yb`→词首、
  `yj`/`yk`→区间首行；只有 `yl`/`yw`/`ye`/`y$` 保持原列）——旧文档“真实 Vim 的 `y`
  不移动光标”**是错的**，据此发出的 `h`→`→`/`b`→`Ctrl+→` 回位键会把光标推回原处，
  `yhp`/`ybp` 随后粘错位置（**缓冲区可见**）。修：
  - `h`/`b`/`B`/`0`/`^`：宿主 `Shift+方向` 正好也把光标带到区间起点 ⇒ **不发键**。
  - `l`：`Shift+→` 前进 1 ⇒ 补 `←` 复位。
  - `j`/`k`：`Up×(n+1)` 回到区间首行（动作被夹取时会过冲，同下面的“动作失败”类）。
  - `w`/`e`：`Ctrl+←` 只在起点本就是词首时才回到原列；**词中/行尾无法恢复**（已知偏差：
    `ywp`@列 12 模型 `one two te\nhree`、Vim `one two threee`）。
  - `$`：宿主在行尾、Vim 保持原列 ⇒ 列无法恢复，只能回到行首（已知偏差）。
- **带计数命令的键码预算（已知偏差）**：发送队列 256 格且**溢出静默丢键**（数据损坏）。
  `x`/`X`/`s` 改为一次选中 N 个字符（N+1 键）、`p`/`P` 只定位一次（±N 键）后已不会撑爆；
  `NJ` 每次 4 键，`99J` 需 392 键 ⇒ **截断到 62 次**（实测 248 键）。
  即使未截断，超出缓冲区末尾的连接在 Vim 里也是 no-op，而宿主的
  `End, Space, Delete` 会真的改缓冲区 ⇒ 属**动作失败**类。
- **带计数的 `Ncc`/`NS` 超出剩余行时多删一个换行**：`4cc` 在 L2 上
  模型得 `L1\n`、Vim 得 `L1\n\n`；在末行 `4cc` 模型删掉末行、Vim 整体放弃。
  根因同上（`Shift+Down×(n−1)` 被夹取到缓冲区末尾之后），属**固有**。
- **行尾删除后的 `x`/`xx`/`xp`**（扩展 §4.4 原来的“行尾删除后的光标”）：删完最后
  一个字符后宿主光标停在**换行上**，故 `xx`/`xp` 会再吃一个换行或在下一行粘贴
  （`xx`@末字符模型 `one two thre`、Vim `one two thr`）；`99x` 同理会跨行（Vim 到行尾就停）。
  宿主无法区分“在字符上”与“在字符后”，属**固有**。
  同类（已声明）：`$x`/`Gx` 的线尾/文末差一（寄存器与光标）。
- **`X` 在列 0（非缓冲区开头）会拼接两行**：`L1\nL2\nL3\nL4\n` 行 1 列 0 上 `X`
  模型得 `L1L2\nL3\nL4\n`、Vim 不变（Vim 的 `X`@列 0 是 no-op）：宿主 `Shift+←`
  在列 0 会**回绕选中上一行的换行**，紧随的 Backspace 就把它删掉。引擎读不到列号，**固有**。
  （旧文档说“列 0 时缓冲区不变”只在**缓冲区偏移 0**时成立。）
- **多行（`N` 行）展开**：先 `Home×2`；`yy` 扩选 `Shift+Down×n`；`dd` 扩选 `Shift+End, Shift+→` + `Shift+Down×(n-1)`（覆盖含换行的 `N` 行）；`cc` 只扩选到 `Shift+End`（**留一个空行**）；`>>`/`<<` 见上（`n=1` 用无选区的 `Home, Tab`）。
- **已知偏差（粘贴/行删的光标）**：`dd` 在**末行**、以及行级 `p`/`P` 之后，宿主光标停在
  **被删/被粘文本的末尾**，而真实 Vim 停在替换行或粘贴文本的**起始行**。
  指令本身的**缓冲区内容正确**；但光标差一行会在**紧接的编辑**下变成缓冲区可见
  （`ddp.`、行级 `p` 后再 `x` 等）。
- **已知偏差（动作失败 ⇒ 操作符应放弃）**：真实 Vim 里 `k`@行0、`j`@末行、`h`/`l`@行首尾等
  **动作失败**时，`d`/`c`/`y`/`>` 会**整体放弃**（no-op）；宿主的方向键只是**夹取**、不报失败，
  于是 `dk`@行0 会删掉当前行、`dj`@末行会删掉末行（**缓冲区可见**）。引擎读不到行号，
  属**固有**。同一根因还导致：动作产生的宿主选区为**空**时（`d0`/`d^`/`d0`@列0、`yh`/`yb`@列0、
  `ygg`@行0），宿主 `Ctrl+X`/`Ctrl+C` 退化成"剪切/复制整行"，而 Vim 是 no-op（= §4.9 的 D16）。
- **已知偏差（行尾删除后的光标）**：在**末字符**上连做 `Nx`/`s`/`C`/`D`/`dw` 时，宿主删完停在
  换行上、Vim 会左移到上一字符，故 `2x`@末字符会多吃一个换行；与 `$x`/`Gx`（`End`/`Ctrl+End`
  落在末字符之后/文末换行之后）同属「宿主无法区分'在字符上'与'在字符后'」的固有类。
- **缩进/反缩进后的光标位置（已知偏差）**：宿主 `Tab`/`Shift+Tab` 把光标留在**编辑点**，而真实 Vim
  把光标留在范围内**首行的首个非空白**。二者在「未缩进行 + `>`」上重合（`>>x` 删首字符），
  以下情形仍有偏差（指令本身的**缓冲区内容正确**，仅光标位置；
  但光标在错行会在**紧接的编辑**下变成缓冲区可见，如 `>wx`/`>Gx`）：
  ① `>G`：光标停在**文末**（宿主无法在选中「当前行→文末」的同时把光标留在当前行）；
  ② `>w`/`>e`/`>W`/`>E`（含计数）：光标停在**移动目标**而非范围首行；
  ③ `<`/`<<` 且该行缩进**未删净**（如 `\t\tL` → `\tL`）：光标停在列 0，Vim 停在列 1（剩余缩进之后）。
- **独立移动 ×n**：`N` 个 `w`/`j`/… 即对应基础序列重复 `n` 次。
- 未列出的 `op+移动` / `缩进+移动` / `op+gg` / `缩进+gg` / `dG`/`>G`/`>0` 等，复用对应基础序列（见 §4.4）。
- **所有 emit 入非阻塞队列**，由 `kv_task()` 按计时发送；**不使用 `wait_ms`**（`pr_boot_combo` 等键盘层保命流程的 `wait_ms` 属键盘层特例，不在此列，详见 [`readme.md`](readme.md) §11 注）。

### 4.9 模式与转移
- **NORMAL**：单键立即 emit；数字→`Cnt`；`d/y/c`→`Op`；`<`/`>`→`Ang`；`g`→`Gp`；`Z`→`Zp`；
  `i/I/a/A/o/O`→INSERT；`v/V`→VISUAL；`Esc`→**交键盘层 Esc 切换**（见下）。
  - 变更类 `s/C/S/c`：进入 Insert（`c` 为操作符，其"改"结果同样进入 Insert）。
- **OP_PENDING（瞬态）**：移动设区间→emit；非期望键→清空+重新识别；Esc→取消。
- **INSERT**：普通字符透传；`Esc` **引擎不再处理**（`kv_kbd` 对 INSERT 一律 `KV_PASSTHROUGH`），
  由共享 keymap 层步骤 6 `esc_process()` 决定：非宽限时吞键转 NORMAL，宽限内透传真实 Esc（见 §4.12）。
- **VISUAL / VISUAL_LINE**：键集 = 移动（含计数 `Nm`）+ `d/y/c/x/s/p`。移动按 Shift 变体扩展选区；
  `d/x`=剪选区、`y`=复制、`c/s`=剪+进 INSERT、`p`=粘贴，完成后回 NORMAL。
  **进入字符级 VISUAL（`v`）时立刻发 `Shift+Right`**（2026-09 修正）：真实 Vim 的 `v`
  马上选中光标下的 1 个字符，因此 `v` + n 次移动 = **n+1** 个字符（`v d` 删 1 个、
  `v l l d` 删 3 个）。旧实现进入时不发键，宿主侧选区只有 n 个字符（`v d` 甚至完全没有
  宿主选区，`Ctrl+X` 退化成"复制整行且不删任何东西"）。
  **复制不得移动光标**（2026-09 修正）：真实 Vim 的 `yy`/`y$`/`Y` 都**保持光标原位**
  （`jyyx` 删的是被复制那一行的字符、`y$p` 在原列粘贴）。旧实现发完 `Shift+Down`/`Shift+End`
  后宿主光标停在下一行/行尾，后续 `x`/`p` 就作用在错误位置（数据损坏级）。
  修：`yy`/`Nyy` 复制后补 `Up×n`；`Y`/`y$` 复制后补 `Home`；`yj`/`yk` 补 `Up×(n+1)`。
  **已知偏差**：行复制必须先 `Home` 到列 0 才能选中整行，故**列位置无法恢复**（恢复到列 0）；
  `yw`/`ye`/`yb` 已按动作回位（`w`/`e`→`Ctrl+←`、`b`→`Ctrl+→`，2026-09 修正，见 §4.4），
  仅当**动作本身未移动**（如行首的 `yh`/`yb`）时回位键会过冲。二者都只影响光标，不影响缓冲区结果。
  **已知偏差**：宿主选区是"半开区间 + 光标在最后一个字符之后"，而 Vim 的 visual 光标停在
  最后一个选中字符**上**，故字符级 VISUAL 下光标位置固有相差 1（缓冲区结果一致）。
  **字符级 VISUAL 的 `$` 含行尾换行**（2026-09 修正，D10）：发射 `Shift+End → Shift+Right`。
  真实 Vim 的 visual `$` 把换行也纳入选区（`v$d` 在 `one two three` 第 3 列上 ⇒ `onfour five six`）。
  **字符级 VISUAL 偏移状态机**（2026-09 新增，缺陷 D1/D12）：宿主选区是半开区间
  `[A+lo, A+hi)`（`A` = 按 `v` 时所在列），宿主光标在 `hi`（`end=R`）或 `lo`（`end=L`）。
  `v` 的预选是 `Shift+Right` ⇒ 进入时 `lo=0, hi=1, end=R`。状态与行选 `s_vl_off` 同构
  （`s_v_lo/s_v_hi/s_v_end_r/s_v_abs/s_v_word_ok`），**所有重置点随 `vline_reset()` 一起清**
  （进入/离开可视、`kv_init`、`kv_set_mode`、`V`/`v` 切换、Esc）。
  - `h`/`l`（含计数）：同向 → 直接 `Shift+方向×n`；反向但**未越过锚点** → 直接收缩；
    **反向越过锚点必须重锚**，否则宿主 `Shift+方向` 会把半开选区塌成空，
    随后 `Ctrl+X` 退化成"剪切整行"（**数据损坏**）：
    - `end=R` 向左越过（`n ≥ w = hi−lo`）：`Esc, Left×(w−1), Shift+Left×(n−w+2)`
      （`Esc` 把选区塌到活动端 = 宿主光标，`Left×(w−1)` 把光标移到 `lo+1`，再扩到目标）。
    - `end=L` 向右越过：`Esc, Right×(w−1), Shift+Right×(n−w+2)`。
    全部是**相对运算**，引擎无需知道 `A` 的绝对列。
  - `0`/`^`（`end=R`）：`Esc, Left×(w−1), Shift+Home` —— 目标列 0 一定 ≤ 锚列，
    直接 `Shift+Home` 会漏掉锚字符（`v0d` 少删 1 字符）。
  - `$`（`end=L`）：先 `Esc, Right×(w−1)` 把光标移回锚字符，再 `Shift+End, Shift+Right`。
  - **前向词动作 `w`/`e`/`W`/`E`（D12）**：`v` 的预选把宿主光标放在 `c+1`，
    直接 `Ctrl+Shift+Right` 会从 `c+1` 起算（差一列）。改为
    `Shift+Left, Ctrl+Shift+Right, Shift+Right`：把光标移回 `c`、从正确列起算、再把目标字符
    纳入半开选区；可连续使用（锚点始终不变）。
  - **后向词动作 `b`/`B`（D19，2026-09 修正）**：宿主光标在 **hi**、而 Vim 光标在 **hi−1**。
    当 Vim 光标恰在**词首**时，直接 `Ctrl+Shift+Left` 会从 hi 回到 hi−1（=锚点）
    ⇒ 半开选区塔成空 ⇒ 随后 `Ctrl+X` **剪切整行**（数据损坏）。
    修：先 `Esc, Shift+Left` 把**锚点挪到 hi**（右端）、宿主光标落到 hi−1 = Vim 光标，
    再 `Ctrl+Shift+Left×n`，与 Vim 的 `b` 逐字对齐（实测 `vbd`/`vby`/`v2bd`/`vbbd`/`vbBd`
    在各列的**缓冲区与寄存器**均与 Vim 一致；修前同些用例会删掉整行）。
    宿主光标已在**左端**（上一动作向左越锚）时锚点本就在右端，直接 `Ctrl+Shift+Left×n` 即可。
    **已知偏差**：若 `b` 落在**已选区域内部**（先向右扩选后再按 `b`，
    Vim 会缩小选区），本实现会以右端为锚重建——方向对但边界不精确（
    `vlbd`@列 0 得 `e two…`、Vim 得 `ne two…`），属**固有**（读不到词边界）。
  - **回退**：`j`/`k`、`b`/`B`、`G`/`gg` 的目标列依赖文本，无法用相对偏移建模 ——
    一律回退到旧的"每步一个 `Shift+方向`"，并把偏移标记为失效（**绝不发错误的重锚**）；
    失效后 `h`/`l` 也回退。
  - **偏移上限 100**（同 `KV_VLINE_MAX_OFF`）：到上限后**拒绝继续扩展**（不发键），
    而不是发出错乱的重锚；计数 ≤99 ⇒ 单条命令 ≤~105 键（发送队列 256 格，见上）。
  - **固有边界限制**：在**列 0** 向左重锚时宿主光标被夹取到列 0，而引擎记的偏移仍按
    "还能继续左移"推算 —— 之后**反向**移动会多算一格（如 `vhld`@列 0 会删错范围）。
    引擎读不到缓冲区边界，属固有限制；P0 复现 `vhd` 本身正确。
  **已知固有偏差（D2）**：`d`/`c`/`y` + **词动作**（`w`/`e`/`b`）在行尾会**吃掉换行** ——
  宿主 `Ctrl+Shift+Right` 会跨行把 `\n` 选进去，而 Vim 的 `w` 不会让操作符删掉换行
  （`dw`@L2 在 `L1|L2|L3|L4` 上 Vim 得 `L1||L3|L4`、引擎得 `L1|L3|L4`）。引擎没有文本知识，
  无法预知词边界，故**固有**；但会静默并/删行，使用前需知悉。
  **字符级 VISUAL 的动作直接作用于当前选区**（2026-09 修正）：
  `d`/`x` = `Ctrl+X`；`y` = `Ctrl+C` + `Esc`；`c`/`s` = `Ctrl+X` + 进 INSERT；`p` = `Ctrl+V` + `Esc`。
  旧实现在 `d`/`y`/`c` 前多发一个 `Shift+End`（把选区扩到行尾，实测 `v l l d` 会删掉整行而不是 2 个字符），
  且复制后不取消宿主选区（下一个键会替换刚复制的内容 —— `yy` 后按 `x` 会删掉整行）。
  **VISUAL_LINE 另有行选近似映射（见下条），其移动输出与 VISUAL 不同。**
  **未列键（数字、`g`、`Z`、`<`/`>`、`i`/`a` 等）为非法键 → 吞键留在 Visual**（不退出、不插入、
  不产生 pending）——即 Visual 模式**没有多键 pending**，`kv_pending()` 在 VISUAL 下恒为 false。
- **VISUAL_LINE（`V`，行选）**——与真实 Vim 行选语义对齐（2026 重写；旧版"锚点固定在被选首行行首 +
  字符级 Shift 扩展"在 `k`/`b`/`gg` 把活动端移到锚点**上方**时会退化成"只选一个换行"：
  `V k d` 会拼接两行、`V gg y` 会丢首尾两行正文，属**数据损坏**，已废弃）：
  > **固有限制**：固件只发按键、读不到编辑器真实选区，也无法获知绝对行号。行选靠引擎自记的**行偏移**
  > `off`（= 光标行 − 锚行 `A`，`A` = 按 `V` 时所在行）**重建**字符级选区；`w`/`e`/`b` 在 Vim 里是
  > **词**动作（同一行内移动时行范围不变），固件按"整行 ±1"近似，属已知偏差。
  - **锚点两种表示**（引擎按 `off` 正负选择，光标始终落在活动端 `A+off`）：
    - `off ≥ 0`（**DOWN**）：锚在 **A 行首**，活动端在 `A+off` **行尾** → 选区 `[A行首, A+off行尾]`。
    - `off < 0`（**UP**）：锚在 **A+1 行首**，活动端在 `A+off` **行首** → 选区 `[A+off行首, A+1行首]`
      （**含 A 行换行**，故复制/删除天然是 linewise）。
  - **进入 `V`**：`Home` + `Shift+End`（选中整行）；`off=0`、方向 = DOWN。从 VISUAL 按 `V` 也走这条
    （真实 Vim 里 `v V` 切成行选）；VISUAL_LINE 内按 `v` 切回字符选（**不发键**，保留宿主选区）。
  - **移动**（`j`/`k` 与计数 `Nj`；`w`/`e` 同 `j`、`b` 同 `k`）：

    | 情形 | 发出 |
    | :--- | :--- |
    | DOWN 态向下 | `Shift+Down`×n + `Shift+End` |
    | UP 态向下、`off_after>0` | **重锚到 DOWN**：`Down`×(−off_before) + `Home` + `Shift+Down`×off_after + `Shift+End` |
    | UP 态向下、`off_after≤0` | `Shift+Down`×n（仍在 A 上方，锚保持 A+1） |
    | UP 态向上 | `Shift+Up`×n |
    | DOWN 态向上、`off_after≥0` | `Shift+Up`×n + `Shift+End` |
    | DOWN 态向上、`off_after<0` | **重锚到 UP**：`Up`×(off_before−1) 或 `Down`×(1−off_before) + `Home` + `Shift+Up`×(1−off_after) |

    - 重锚**只在方向翻转**时发生，且**直接从当前光标**重建（不做"先按 `Shift+↑/↓` 移动、再重锚"
      的冗余移动）。后者会把键码数抬到 ~3n：`V 99 k` = **300 键** > 发送队列 256 格
      （`emit.c` 的 `EMIT_CAP`），溢出时**静默丢键** → 选区错乱 → 后续 `d`/`y` 作用在错误范围
      （**数据损坏**）。现在的键码数：同向 = n+1、重锚 ≤ n+3、`gg`/`G` ≤ |off|+1。
    - **行选跨度上限 `|off| ≤ 100`**（`KV_VLINE_MAX_OFF`）：到上限后同向移动**不再发键**
      （选区停止扩张，而不是错乱）。取 100 使单条命令 ≤102 键，与 Normal 的 `99dd`(103) 同量级，
      也与 Normal/Visual 的"2 位计数 ≤99"语义一致。
    - 这样任一时刻的宿主选区都**覆盖完整整行**，与 Vim 的"锚行固定、行范围随光标"一致。
  - **不改行范围的键**：`h`/`l`/`0`/`^`/`$` 在行选下**不发任何键**（真实 Vim 里它们只移动光标、
    行范围不变；发 `Shift+←/→` 反而会把活动端带出本行、把邻行卷进选区——旧版缺陷）。
  - **`G` / `gg`**（绝对位置，行号未知 → 之后 `off` 失效，置 `abs`）：

    | 情形 | 发出 |
    | :--- | :--- |
    | `G`（DOWN 态） | `Ctrl+Shift+End` |
    | `G`（UP 态） | `End` + `Down`×(−off) + `Home` + `Ctrl+Shift+End` |
    | `gg`（UP 态） | `Ctrl+Shift+Home` |
    | `gg`（DOWN 态） | `Up`×max(0,off−1) + `Down`×max(0,1−off) + `Home` + `Ctrl+Shift+Home` |

    - 与 Vim 一致：`gg` 光标移到文首、`G` 移到文末，范围 = `[文首, A]` / `[A, 文末]`。
    - `abs` 之后 `off` 未知：之后的行移动只做纵向扩展，并按**活动端所在边界**收边 ——
      `s_vl_up`（`gg` 之后，活动端是**上**边界）用 `Shift+Home`；否则（`G` 之后，活动端是**下**边界）
      用 `Shift+End`。这样「`gg` 后向下 / `G` 后向上」这类自然方向仍然正确
      （`V gg j y` = 复制 L2..L3，与 Vim 一致；旧版误用 `Shift+End` 会退化成只选一个换行，
      `V gg j d` 会把两行拼接 = 数据损坏）。
    - **`abs` 状态下反向越过锚行仍会出错**（`V G k k k y` 得到空串、`V gg j j j y` 丢正文；
      用 `d`/`x` 会删错范围）：越过锚行需要重锚，而重锚必须知道 `off`，`G`/`gg` 之后 `off` 已丢失。
      **这是固有限制**（纯键码无法表示「第 N 行」），请避免 `G`/`gg` 之后再反向越过锚行。
  - **动作**（方向无关，选区形状由引擎保证）：
    - 先补行尾换行：**DOWN 态**发一次 `Shift+Right`（把活动端推过行尾换行，使复制/删除是 linewise）；
      **UP 态**选区已含换行，不再补。
    - `y` = `Ctrl+C` + **`Esc`**；`d`/`x` = `Ctrl+X`；`c`/`s` = `Ctrl+X` + `Shift+Enter`
      （Vim 的 `V c`/`V s` 都是"删掉整行、留一个空行、进 Insert"，故二者**完全等价**）；
      `p` = `Ctrl+V` + `Esc`（用寄存器覆盖选区）。
    - **`Esc` 收尾**：宿主在 `Ctrl+C` 后通常保留高亮选区，不取消则下一个按键会替换刚复制的行；
      Vim 复制后也会取消选区，故 `y`/`p` 后补发 `Esc`。`Ctrl+X` 已删除选区，无需补。
    - 动作后模式：`y`/`d`/`x`/`p` → NORMAL，`c`/`s` → INSERT（宿主本就在 Insert，`Shift+Enter` 即
      "留一个空行"）。
  - **用户按 `Esc`**：退出行选并发一次 `Esc` 取消宿主残留选区（Vim 同）。
  - **已知偏差（需实机确认）**：① `w`/`e`/`b` 按「整行 ±1」近似（Vim 是**词**动作，同一行内移动时行范围
    不变）；② 动作后光标停在活动端，Vim 停在选区首行；③ 末行无换行时 `Shift+Right` 无效：`V d` 会留
    一个空行、`V y` 的寄存器也少一个换行；④ **`abs` 状态下反向越过锚行会出错**（见上；用 `d`/`x` 会
    删错范围 = **数据损坏**）；⑤ `V y` 后按 `p`：寄存器已是 linewise，但 Normal 的 `p` 只发 `Ctrl+V`
    （在插入点粘贴），与 Vim「在下一行新建一行粘贴」仍有差距；⑥ **行选跨度上限 100 行**（发送队列只有
    256 格，见上；Vim 无此限制）；⑦ `3G`/`3gg` 的计数被丢弃（纯键码无法表示「第 3 行」，`V 3G y` 取到
    文末而非第 3 行）；⑧ 可视 `p` 不更新宿主剪贴板（Vim 会把被替换的文本放进无名寄存器 —— 单剪贴板
    无法同时保存「要粘贴的」与「被替换的」）；⑨ `V` 后按 `v` 切字符选时宿主选区仍是整行（Vim 收成
    1 字符，`V v y` 复制 `L1` 而非 `L`）；⑩ `V j` 期间选区不带行尾换行（动作会补 `Shift+Right`，无影响）。
  - **已知偏差（D14–D18，2026-09 独立审查登记；⑪–⑮ 为字符级 VISUAL / Normal / keymap 层）**：
    ⑪ **D14**：`$x`/`Gx` 的行尾/文末差一。`$`⇒`End` 把宿主光标留在末字符**之后**（或换行上），
    `G`⇒`Ctrl+End` 落在文末换行**之后**的空行；宿主键码无法表达"在末字符上"与"在它之后"的区别。
    字符级 visual `$` 在**末行**也因此多删一个换行（非末行正确，见 D10）。**固有**。
    ⑫ **D15**：`J` 在下一行为空时多插一个空格（Vim 不插）；且 Vim 会去掉下一行的前导空白，
    引擎读不到其长度。**固有**。
    ⑬ **D16**：字符级 VISUAL 的移动是 **no-op** 时（如列 0 的 `yh`、行 0 的 `ygg`）宿主选区为空，
    `Ctrl+C` 退化成"复制整行"（剪贴板污染），而 Vim 放弃操作符。**固有**。
    ⑭ **D17**：`cgg`@行 0 —— 纯键码无法判断"已在第 1 行"，理论上是 D1 同类的锚点方向问题；
    但现有 `c`+行选动作的 `Shift+Enter`（§4.4）已补出空行，实测 `cgg`@(0,0) 得 `|L2|L3|L4`
    与 Vim 一致，故**已不构成偏差**，此条仅保留以便审计可追溯。
    ⑮ **D18**：`i<Esc>x` 差一 —— Vim 的 `Esc` 把光标左移一格，宿主插入光标不左移。
    这是 **keymap 层**的事（INSERT→NORMAL 需补发 `Left`），**在引擎范围外**，仅在此登记。
  - **与 VISUAL 的关键差异**：`VISUAL` 的移动一律 `Shift+方向`/`Ctrl+Shift+方向`（字符/词级）；
    `VISUAL_LINE` 的 `w`/`b`/`e` **改为整行推进**，`h`/`l`/`0`/`^`/`$` **不发键**，动作前会按方向补
    `Shift+Right` 并重锚，因此同一串按键在两模式下**输出不同**、选区形态也不同（这是本规格的可测断言）。
  - **动作后的模式**（与 Vim 一致，VISUAL 与 VISUAL_LINE 相同）：
    `y`（复制）与 `d`/`x`（删除）**执行完回 NORMAL**；`c`/`s` 回 NORMAL 后进 INSERT；
    `p` 粘贴后回 NORMAL；`Esc` 回 NORMAL。
- **MOUSE**：键盘层模式（引擎一律 `KV_PASSTHROUGH`，见 §4.7）。**右 Alt 短按**（阈值 **200ms**，与
  Caps 一致）在 `Insert`/`Normal`/`Visual` 均可进/出（长按=RAlt 修饰）；**进出 MOUSE 视同模式切换，
  先清 pending**。模式内：`hjkl`=指针、`Shift+J`/`Shift+K`=滚轮下/上、`Space`=左键（短按单击/长按
  拖动）、`Enter`=右键；**`Shift` 不触发退出**（press 吞、release 透传，供滚轮组合）；**`Ctrl`/`Alt`/`GUI`
  按下即退出**（强制反注册全部按住的鼠标键/轴后，在进入前模式**重新识别该修饰键**，其 release 随后透传）；
  **其它非修饰键**退出 MOUSE 并**强制反注册全部按住的鼠标键/轴**（指针四向、左右键、滚轮）后，
  在进入前模式**重新识别该键**；`Esc` 在 MOUSE 内同此规则（退出+重识别，随后由 `esc_process` 按新规则处理）。
  RGB 指示为**青**（详见 [`readme.md`](readme.md) §8）。

### 4.10 修饰键、key-up 与输入保真
- **key-up 一律透传**；唯一例外是按住连发移动 `h/j/k/l`（down `register` 宿主方向键 / up `unregister`，
  由 glue 执行，且**与当前模式无关**——切换模式/禁用 vim 时 glue 必须反注册已按住的方向键）。
- **引擎返回 `KV_CONSUMED` 的 press，由 glue 记账并吞掉对应 release**（held motion 例外）；
  返回 `KV_PASSTHROUGH` 的 press，其 release 也必须透传给宿主——**press 与 release 的归属必须配对**，
  否则宿主收到孤立 release（E3 的镜像）。
- **keymap 层消费 press 的键，其 release 也须一并消费**：keymap 前置分支（如 `Shift+Esc` 组合）在
  按下时消费了某键，必须记住并**无条件吞掉其抬起**（应经 glue 的统一配对表，见 §4.12）。
- **非 vim 键码一律透传**。**注意**：可视模式内已累积的输入（计数位数 **或** `g` 前缀）必须在**每一处**
  可能截断输入的路径上作废，否则 `v 3 F5 j` / `v g F5 g` 这类序列会把计数/前缀泄漏给后面的按键
  （见 §4.9 实现要点）。引擎提供两个接口：
  - `kv_visual_count_pending()`：可视计数或 `g` 前缀是否待用（glue/keymap 的判定条件）；
  - `kv_visual_cancel()`：作废可视输入（等价 `kv_cancel()`：清计数/前缀并丢弃 repeat 记录）。

  **四处调用点**（缺一即漏洞）：
  1. glue 的 CAG 分支（带 Ctrl/Alt/GUI 的透传键，`vim_glue.c`）；
  2. glue 的非 vim 键透传分支（F 键、层键、普通打字等）；
  3. keymap 共享层的 **myfn 吞键**路径（未声明键被吞时，`vim_keymap_common.c` 步骤 4）；
  4. keymap 共享层的 **hook 吞键**路径（`hook_pre`/`hook_post_myfn` 消费的键，如 CAD / Fn+Esc）。

  统一写法：`if (kv_pending() || kv_visual_count_pending()) kv_visual_cancel();`

  **例外（规格明确）**：**纯修饰键**（`Shift`/`Ctrl`/`Alt`/`GUI` 自身的按下/抬起）**不作废**可视输入——
  它们是"与后续键组合"的暂态，用户按住 Shift 再按 `j`（`Shift+j`）是合法组合，若按住 Ctrl 就清掉
  计数会让 `3` 之后的组合无法完成。`Ctrl+<key>` 这类**带修饰的透传键**仍按 CAG 分支作废计数
  （因为它是完整的"非 vim 键"，会截断输入）。
- **修饰键影子**：glue 维护**物理**修饰键影子（记录每个修饰键的物理 down/up，不依赖 `get_mods()`，
  免受 oneshot/锁存干扰），用于 bootloader 组合判定与 Shift 折叠；**不打包、不 `clear_mods`/`set_mods`**
  （键盘层"剥修饰发裸键"属例外，见 §2.1，需临时 clear 并恢复）。
- **右 Shift 懒发送（glue）**：vim 开启时，物理右 Shift **单独按下/抬起不注册任何键**（孤立 Shift 会
  触发宿主输入法切换）。当右 Shift 按住期间有其它键（非修饰键、非 Esc、非层键）透传时，glue 才**临时
  补注册左 Shift**，并保持到右 Shift 抬起再反注册——因此 `右Shift+a`=`A`、`右Shift+Ctrl+C`=`Ctrl+Shift+C`，
  且宿主永不见孤立 Shift。右 Shift 的 press/release 由共享配对表吞掉。修饰键/Esc/层键豁免（不包装）。
  vim 关闭时右 Shift 为普通修饰键。`vim_glue_mods()` 影子**照常记录右 Shift**，故 Normal 下的 Shift 折叠
  （`右Shift+p`→`P`）与 `右Shift+Esc`→`` ` `` 仍成立。
  **实现约束（2026-09 审核修正）**：①补注册的判据必须是**当前报告位**（`get_mods() & MOD_BIT_LSHIFT`），
  不能只看"懒 Shift 已置位"的闩锁——物理左 Shift 按下再松开会清掉共享位而闩锁仍为真，此后右 Shift
  就再也补不上 Shift；②反注册（右 Shift 抬起、或模式/使能切换的 `vim_glue_release_all()`）必须只在
  **没有物理左 Shift 占位**时才执行，否则会把物理按住的 Shift 一起清掉；③`Esc` 属豁免集合，
  **不得**被懒 Shift 包住（否则 Normal 下的 `右Shift+Esc` 会变成宿主的 `Shift+Esc`）。
- **Caps 模式的 Ctrl 是**位模型**（2026-09 审核修正）**：LCTL 与 RCTL 在真机是**不同 bit**，且
  `register_code`/`unregister_code` 是**无引用计数**的裸位操作。因此：
  ①进入模式时按**实际按住的那一侧**逐位记录 `s_caps_phys_ctrl_held`（不能"按住任一侧就把两侧都记为物理在位"，
  否则"物理按住右 Ctrl 时进入"会留下永不清除的合成左 Ctrl 位）；②物理 Ctrl 的 release 必须先
  `vim_glue_pair_drop()` 再清 `owned` 闩锁——若该 press 曾被 held 表溢出吞掉，其 release 会被配对表消费、
  永远到不了 QMK 的 `del_mods`；③合成位的反注册判据一律**逐位**（与退出路径一致）。
- **emit 非阻塞**：`kv_task()` 按计时发送；不使用 `wait_ms`。

### 4.12 glue 层（QMK 适配层）职责规格
位置：`qmk-vim-fn/qmk/vim_glue.{h,c}`（依赖 QMK API；**所有键盘共用，禁止在 keymap 里复制实现**）。

**接口**（`vim_glue.h`）：
```c
void vim_glue_init(void);                 /* kv_init + kv_set_emit + 影子/配对表复位 */
void vim_glue_mod_update(uint16_t keycode, bool pressed); /* 物理修饰键影子更新 —— pipeline 第 0 步，
                                                             必须先于一切吞键（myfn 会吞修饰键） */
uint8_t vim_glue_mods(void);              /* 影子（QMK 打包位）只读查询 */
bool vim_glue_engine(uint16_t keycode, keyrecord_t *record); /* 尾段引擎分发：Shift 折叠/CAG 透传/
                                                                 kv_kbd/配对/release 处理/held motion；
                                                                 返回已按 QMK 极性（true=放行） */
void vim_glue_swallow(uint16_t keycode);  /* 键盘层前置分支消费 press 后调用：release 由 glue 统一吞 */
void vim_glue_task(uint32_t now_ms);      /* 排空 kv_task */
void vim_glue_release_all(void);          /* 反注册 held motion 方向键（禁用/切模式/进 MOUSE 时） */
```

**职责清单**（对 §4.7/§4.10 的落地）：
1. **key-down 分发**（在 `vim_glue_engine` 内）：纯 Shift → 折叠为 `KV_C_*` 喂引擎；带 Ctrl/Alt/GUI →
   不喂、放行 QMK——**例外**：Normal 下**裸 `h/j/k/l`**仍作方向键（与所按 Ctrl/Alt/GUI 组合，如
   `Win+h`→`Win+←`），pending 前缀（`3l`/`dl`）仍走严格透传；**Esc 不做任何键盘层处理**（引擎已实现
   pending 取消/Visual 退出；Insert/Normal 的 Esc 切换由共享 keymap 层 `esc_process` 负责）。
   **右 Shift 懒发送**（见 §4.10）：单独不注册，按它键时临时补左 Shift。
2. **统一 press/release 配对表**：引擎 CONSUMED 与键盘层 swallow 的键共用**同一张表**
   （keymap 禁止再自建 swallow 旗标/数组）；表满策略：最旧条目被覆盖（新键优先）。
3. **held motion**：`h/j/k/l` 的 register/unregister；切换模式/禁用/进 MOUSE 时强制反注册。
4. **物理修饰键影子**：`vim_glue_mod_update` 在 pipeline **第 0 步**调用——NUT65 的
   `Fn+右Shift+Esc` bootloader 组合中 RSFT 会被 myfn 吞键，`get_mods()` 不可靠，影子必须
   先于吞键更新；供 bootloader 判定、Caps/鼠标的 tap/hold 修饰查询。
5. **emit→QMK**：`register_mods(转换后 HID 位)+register_code+unregister`，不写回 mods 报告。
6. **极性封装**：`vim_glue_engine` 返回值已按 QMK 极性（true=放行），keymap 不再手写 `!`。

**共享 keymap 层**（`qmk/vim_keymap_common.{h,c}`；两键盘共用，禁止在 keymap 复制实现——
历史 bug 全在此段；QK61 现行实现与 NUT65 V1.0 逐行比对确认以下均为 spec 级）：
- **`vim_pipeline_process(keycode, record, cfg)` — 单源拦截链**（取代两键盘各自手写的十段顺序）：
   ```
   0 影子更新(vim_glue_mod_update)     ← 先于一切吞键（myfn 吞修饰键后 get_mods 失效）
   1 Caps 触发(caps_process)           ← 按下即进 Caps 模式；Fn+Caps 单击=开关 vim；裸 Caps 无效果
   2 Caps 模式拦截(caps_mode_process)   ← 模式内接管一切按键（caps/design.md §4）：F 区/Ctrl+键/层键豁免
   3 cfg->hook_pre                     ← NUT65: pr_boot_combo(影子判定)/电源组合；QK61: NULL
   4 myfn 骨架                         ← 层键豁免(fn 1.4.0)+未定义(含修饰键)吞键+已声明调 cfg->myfn
   5 cfg->hook_post_myfn               ← QK61: 闪灯/Ctrl+Alt+Del/Fn+Esc 复位；NUT65: NULL
   6 鼠标模式状态机                     ← 见下（参数化 vim_cfg_t）
   7 Shift+Esc(cfg->shift_esc_enable)  ← LSFT+Esc=~/RSFT+Esc=`(仅 Insert)；RSFT 由 glue 懒发送剥离
   8 Esc 切换(esc_process)             ← Insert<->Normal 切换 + 3s 宽限（仅 Normal->Insert 开启，窗口内重置）
   9 §2.1 快捷键表                     ← 两键盘完全一致(BSPC/Space/-/Shift+=/Ctrl+F/B//)：
                                          base+mods 匹配+kv_cancel 前置+send_plain_tap
  10 vim_glue_engine                   ← 右 Shift 懒发送 + 引擎分发（Insert Esc 直落透传）
   ```
   每段显式命名+前置条件注释（消除 A-P1-7 隐式顺序契约）。
- **鼠标模式状态机**（参数化；enter/exit、200ms 短/长按、`hjkl`/`Shift+J`/`Shift+K`/`Space`/`Enter`
  的 press/release 按"实际注册键"配对、`Shift` 不退出、`Ctrl`/`Alt`/`GUI` 按下退出+重识别、
  非修饰键退出强制释放全部鼠标键+重识别）：
  ```c
  typedef struct {
      uint16_t trigger_kc;      /* QK61=QK_KB_22；NUT65=右 Alt 位自定义键 */
      uint16_t mod_win, mod_mac;/* 长按修饰 KC_RALT/KC_RGUI */
      bool   (*link_ok)(void);  /* NUT65=mouse_link_ok；QK61=NULL 恒真 */
      uint16_t hold_ms;         /* 200 */
      bool     shift_esc_enable;/* Shift+Esc 组合开关 */
  } vim_mouse_cfg_t;            /* 仅示意鼠标子集；实际传入的统一类型是 vim_cfg_t */
  static bool mouse_process(uint16_t kc, keyrecord_t *r);
  static void mouse_release_all(void);
  ```
  > 上面是**鼠标子集**的视图；实际传入共享层的完整结构是 `vim_cfg_t`（`qmk/vim_keymap_common.h`），
  > 它还带 `led_index`（模式色灯位）与 `insert_flash_color`（`Normal--Esc-->Insert` 3s 提示色，见下）。
- **tap/hold 计时 helper**（Caps/鼠标触发键/Fn+Esc 3s 等**一切长按判定共用**，含 timer 防零）；
  > **Esc 宽限窗口用 32 位计时**（`vim_timer_start32()` / `timer_elapsed32()`）：QMK 的 `timer_read()` 是
  > `(uint16_t)timer_read32()`，16 位比较在 **65536ms 处回绕**——已过期的窗口会重新被判为有效
  > （持续打字 65.5s 后出现 3s 假命中）。凡窗口长度可能被"长时间不检查"跨越的计时，一律 32 位。
- **`send_plain_tap(kc)`**："剥修饰发裸键"（§2.1 例外：临时 clear+恢复）；
- **`vim_task(now_ms)`** = `vim_glue_task` + 鼠标长按检查（拖动/长按修饰进入）；
- **`vim_rgb_state_color(enabled, m, pending, mouse, &r,&g,&b)`**：状态色计算 —— spec 级；
  键盘只提供**灯位索引**（`cfg->led_index`）。**七色**与优先级（高 → 低）：

  | 条件 | 颜色 | 值 |
  | :--- | :--- | :--- |
  | 鼠标模式（`mouse`，最先判定） | 青 | `#00FFFF` |
  | vim 关闭 | 红 | `#FF0000` |
  | Visual（`KV_MODE_VISUAL`） | 紫 | `#800080` |
  | **Visual-Line（`KV_MODE_VISUAL_LINE`）** | **洋红 rose** | **`#FF0080`** |
  | Normal + `pending` | 黄 | `#FFFF00` |
  | Normal（空闲） | 蓝 | `#0000FF` |
  | Insert（及其它/默认） | 绿 | `#00FF00` |

  - **VISUAL 与 VISUAL_LINE 必须是两种颜色**（紫 / 紫红）——行选是独立模式，灯色要能区分。
  - `pending` **不覆盖** Visual / Visual-Line（Visual 内无多键 pending，见 §4.9）；
    Mouse 与 vim-off 仍优先于一切模式色。
- **`vim_insert_flash(void)`**：`Normal --Esc--> Insert` 的「回到打字」提示色窗口判据（spec 级）。
  定义：**vim 已开启 + 当前模式为 INSERT + §4.12 步骤 6 的 Esc 宽限窗口（`VIM_ESC_GRACE_MS` = 3000ms）
  仍未过期** 时为真。该窗口**只**由 `esc_process()` 的「Normal 空闲 Esc → INSERT」开启、由窗口内的 `Esc`
  重置、并在模式离开 INSERT 时被 `vim_pipeline_process()` 清掉；因此该判据等价于「Insert 是由
  Normal 空闲 Esc 进入的（且 3s 内）」，`i`/`a`/`o`/`s`/`c`、开机、`Caps` 开启 vim 等入口均为假。
- **`vim_insert_flash_color(&r,&g,&b)`**：把上面的判据与 `cfg->insert_flash_color`（`0xRRGGBB`；
  `0` = 不覆盖）**一起裁决**——命中且色值非零时拆出色分量并返回 `true`，键盘据此**替换**
  `vim_rgb_state_color()` 给出的 Insert 绿；返回 `false` 时键盘保持模式色。**键盘不再自己读该字段**
  （避免"文档写了 cfg 契约、代码却用局部宏"的漂移）；替换范围仍由键盘决定
  （QK61：Esc 灯 + logo 电量灯；NUT65：Esc 灯 + 底部电量灯条）。
  优先级：MOUSE 青、Visual 紫、Normal 蓝、pending 黄、vim 关红**均不受影响**（该判据只可能为真于 Insert）。
- **myfn 骨架**：层键豁免；未声明键（含修饰键）press 吞、release 由配对表裁决；**已声明键调 `cfg->myfn(kc,pressed)`**，
  返回 `true`=消费（press 入配对表、release 交配对表）、`false`=放行给 QMK（F 区/音量、NUT65 厂商 `EE_CLR`/`BT` 等）。

**键盘层保留**（真·键盘专属）：RGB **灯位索引**、vendor 组合键**骨架**（Fn+Esc 复位、bootloader、
CAD 的触发检测+厂商调用）、底排键位与触发键**定义**、VIA、`vim_cfg_t` 实例的填写。
> vendor 组合键只留骨架：Fn+Esc 复位在 QK61 属 keymap 自建、在 NUT65 由厂商 `nut65.c` 全权
> （`_FN[0,0]=EE_CLR` 真键码+厂商 3s 计时，keymap 零行）——两家不在同一层，共享骨架只服务一家故不抽；
> 但其 **press/release 配对必须走 glue 统一配对表**（禁止自建旗标，A-P0-2）、**长按计时必须用共享
> tap/hold helper**。bootloader 判定一律读**影子**（`vim_glue_mods()`），不读 `get_mods()`。

### 4.11 性能与安全评估

**结论**：
- **安全**：结构性根治 E2/E3/A1/A2/A3/A6 + 死键 + pending 滞留；新风险为 held motion 例外、
  重新识别不得重复 emit、修饰键影子需覆盖 oneshot/layer。
- **性能**：解析层与修饰键层明确改善（省每键改装修饰键、省 key-up 处理链）；
  emit/阻塞层**不自动改善**，须依赖非阻塞队列。

**性能对照**

| 维度 | 现 qmk-vim | 新引擎 | 改善 |
|---|---|---|---|
| 每键解析 | 函数指针链 + 大 switch | 队列 + classify + 表查 + handler | 相当/略优 |
| 修饰键 | 每键打包 + `clear_mods`/`set_mods` | 物理影子只读 | **明确改善** |
| key-up | 走完整链且 `return false` | 忽略（held motion 例外） | **约省一半事件** |
| Insert 打字 | 需专门快路径 | 天然透传 | 改善 |
| 阻塞 | `u`/重做 `wait_ms(50)`、`clear_keyboard` | 非阻塞队列 + 不清键盘 | 依赖 #7 实现 |
| emit 报告数 | `dd`=10 报告 | 固定映射继承同序列 | 无改善 |

**安全对照**

| 实案 | 现根因 | 新引擎 | 判定 |
|---|---|---|---|
| E2 幽灵修饰键卡 Shift | 无条件 `set_mods` | 不打包不回写 | 根治 |
| E3 Alt+Tab 卡 Tab | 吞 key-up | key-up 一律透传 | 根治 |
| A1 计数泄漏 | 全局计数清理分散 | 计数限移动/缩进/行操作 + ctx 严格重置 | 根治 |
| A2 `.` 卡 pending | 模式耦合 | 命令 token 记录 + 显式模式 | 根治 |
| A3 NKRO 卡 motion | `clear_keyboard` | 不清键盘 | 根治 |
| A4 let-through 取消过宽 | `process_func != normal` 误判 | 模式/pending 显式 | 根治 |
| A6 混合修饰符打包错 | 位移打包 | 不再打包 | 消失 |
| 死键 | 未处理键静默吞 | 非 vim 键透传 | 根治 |
| E1 dd 编辑器依赖/末行 | 固定键序列 | 继承同序列 | 不变 |
| E4 dd 双撤销 | 两次主机编辑 | 方案 A：取消双撤销 | 变（一次 u 半恢复） |
| A5 可视文本对象取消 | 状态耦合 | 文本对象已整体剔除 | 消失 |
| A7 计数上限 | 名义 ≤2 位，存在越界路径 | 上限 2 位（≤99），第 3 位起忽略 | 根治 |
| A8 直接映射模键码 | 打包修饰位 | 不再打包，物理影子 | 消失 |

---

## 5. 与现实现差异对照

| 项 | 现 qmk-vim | 新引擎 |
|---|---|---|
| pending 遇非期望键 | 操作符=取消+重处理；文本对象/g 前缀=吞键 | **统一：清空 + 重新识别；非 vim 键透传** |
| 计数模型 | 数字拼接（`2d3w`→`23w`） | **相乘（`2d3w`=`d6w`）** |
| 计数作用域 | 移动与 `dd/cc/yy` | 统一为移动与行操作 |
| 小写 `x`/`s` | 有 | 保留 |
| `J` | keymap 层 `Shift+J` | 引擎接管 |
| 缩进 `<` `>` | 无 | **新增** |
| replace 模式 | 有（`R`） | 取消；`R`/`Shift+R` 透传 |
| Esc 长按 | 长按→Normal | 去除长按 |
| dd 双撤销 | `vim_extra_undos` | 方案 A：取消 |
| 鼠标模拟 | 方向键 + Space + End | 改为右 Alt 短按切换鼠标模式（长按=RAlt；任意模式可进；指示青） |
| 文本对象 `iw/aw` | 有 | 剔除 |
| 搜索/标记/宏/寄存器 | 部分 | 剔除 |
| key-up | 吞 | 透传（held motion 例外） |
| 修饰键 | 打包 + clear/set | 物理影子 |
| `.` | keycode 缓冲回放 | 命令 token 回放 |

---

## 6. 落地里程碑

1. `engine/` 骨架 + queue + classify + 测试框架跑绿
2. NORMAL 单键 + 基础移动（w/e/b/x/gg/G）+ held motion（按下/释放）
3. 操作符/移动 + 前缀计数（含前后相乘）+ 严格清空 + 非阻塞 emit（`kv_task`）
4. 缩进 `<`/`>` + `g`/`Z` 前缀
5. INSERT（i/a/o、Esc）
6. repeat `.`
7. VISUAL / VISUAL-LINE
8. 主机测试全绿（含 E2/E3/A1/A2/A3 回归）

**范围外（后续阶段）**：接回固件替换现 `process_func`、鼠标模式的**固件接入**（行为已定稿，见 §4.9 与 [`readme.md`](readme.md) §8）、Caps/Esc 交互、
合并 qmk-vim + qmk-myfn、去上游依赖。
