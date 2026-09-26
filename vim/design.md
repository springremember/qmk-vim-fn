# vim 引擎：技术架构与实现细节

> 面向实现。描述 vim 引擎的**目标架构**（按模式解耦 + token 队列 + 表驱动 + pending 严格清空），
> 借鉴 Vim 源码的语法模型，并针对固件"只能发键序列、不能改缓冲区"的现实设计。
> 使用说明见 [`readme.md`](readme.md)；变更与历史问题见 [`changes.md`](changes.md)；
> 测试见 [`testcase.md`](testcase.md)。
>
> 注：本文件为重构设计的**唯一权威**，取代 `qmk-vim/docs/REDESIGN_PLAN.md`（历史草案，勿引）。
> 注：文中出现的键盘名（如 QK61/NUT65）仅为**参考示例**；本仓库共享层不含任何键盘专属实现或测试。
> 注：**变更流程（文档先行）与共享层↔键盘分支的同步/验证规范**见 [`../qmk/README.md`](../qmk/README.md)；
> 该规范与本文档同级权威，改动顺序、子模块同步、验证清单、坑位清单均以其为准。
> 「文档先行」认可的文档文件集：`vim/{design,readme,changes,testcase}.md`、`fn/{readme,changes}.md`、`qmk/README.md`。

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
- `dd`（定稿）：`Home×2 → Shift+End → Ctrl+X → Backspace`（两次主机编辑）。
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
| 2b | 计数作用域 | 仅移动、缩进与行操作；其余键丢弃计数。`G`/`gg` 虽属移动，**任何上下文都丢弃计数**（`2dG`≡`dG`、`2dgg`≡`dgg`）；操作符/缩进后的 `0` 亦丢弃 n（`2d0`≡`d0`） |
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
                             // vim_task、vim_rgb_state_color 六色计算
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
| `x` / `X` | Delete / Backspace |
| `s` | Shift+→, change |
| `C D Y` | `c$` / `d$` / `y$` |
| `S` / `NS` | 同 `cc` / `Ncc`（**×n 行**） |
| `dd` / `Ndd` | Home, Home, Shift+End, Shift+Down×(n-1), Ctrl+X, Backspace（**×n 行**；n=1 时无 `Shift+Down`） |
| `yy` / `Nyy` | Home, Home, Shift+Down×n, Ctrl+C（**×n 行**） |
| `cc` / `Ncc` | Home, Home, Shift+End, Shift+Down×(n-1), change (+Insert)（**×n 行**；n=1 时无 `Shift+Down`） |
| `dw` / `d$` / `d0` | 选词/选到行首尾 → Ctrl+X |
| `p` / `P` | Ctrl+V（`yanked_line` 定位） |
| `J` | End, Delete |
| `u` | Ctrl+Z（单次） |
| `ZZ` | Ctrl+S |
| `i I a A o O` | 见下 |
| `> <` | 缩进 / 反缩进（`>0`/`<0` = 缩进/反缩进到行首） |

- 插入：`i` 原地；`I`=Home 后；`a`=→ 后；`A`=End 后；`o`=End,**Shift+Enter**；`O`=Home,**Shift+Enter**,↑。
- 粘贴定位：`yanked_line` 为真时 `p` 先 End+→，`P` 先 End+→+↑；否则 `P` 先 ←。
- **多行（`N` 行）展开**：先 `Home×2`；`yy` 扩选 `Shift+Down×n`；`dd`/`cc` 扩选 `Shift+End` + `Shift+Down×(n-1)`（覆盖含换行的 `N` 行）；`>>`/`<<` 同理按行扩选，再执行对应动作。
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
  **未列键（数字、`g`、`Z`、`<`/`>`、`i`/`a` 等）为非法键 → 吞键留在 Visual**（不退出、不插入、
  不产生 pending）——即 Visual 模式**没有多键 pending**，`kv_pending()` 在 VISUAL 下恒为 false。
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
- **非 vim 键码一律透传**。
- **修饰键影子**：glue 维护**物理**修饰键影子（记录每个修饰键的物理 down/up，不依赖 `get_mods()`，
  免受 oneshot/锁存干扰），用于 bootloader 组合判定与 Shift 折叠；**不打包、不 `clear_mods`/`set_mods`**
  （键盘层"剥修饰发裸键"属例外，见 §2.1，需临时 clear 并恢复）。
- **右 Shift 懒发送（glue）**：vim 开启时，物理右 Shift **单独按下/抬起不注册任何键**（孤立 Shift 会
  触发宿主输入法切换）。当右 Shift 按住期间有其它键（非修饰键、非 Esc、非层键）透传时，glue 才**临时
  补注册左 Shift**，并保持到右 Shift 抬起再反注册——因此 `右Shift+a`=`A`、`右Shift+Ctrl+C`=`Ctrl+Shift+C`，
  且宿主永不见孤立 Shift。右 Shift 的 press/release 由共享配对表吞掉。修饰键/Esc/层键豁免（不包装）。
  vim 关闭时右 Shift 为普通修饰键。`vim_glue_mods()` 影子**照常记录右 Shift**，故 Normal 下的 Shift 折叠
  （`右Shift+p`→`P`）与 `右Shift+Esc`→`` ` `` 仍成立。
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
   1 cfg->hook_pre                     ← NUT65: pr_boot_combo(影子判定)/电源组合；QK61: NULL
   2 myfn 骨架                         ← 层键豁免(fn 1.4.0)+未定义(含修饰键)吞键+已声明调 cfg->myfn(返回 bool:消费/放行)
   3 cfg->hook_post_myfn               ← QK61: 闪灯/Ctrl+Alt+Del/Fn+Esc 复位(3s 用共享 hold helper，
                                          配对走 glue 表)；NUT65: NULL
   4 鼠标模式状态机                     ← 见下 vim_mouse_cfg_t
   5 Shift+Esc(cfg->shift_esc_enable)  ← LSFT+Esc=~/RSFT+Esc=`(仅 Insert)；RSFT 由 glue 懒发送剥离
   6 Esc 切换(esc_process)             ← Insert<->Normal 切换 + 3s 宽限（仅 Normal->Insert 开启，窗口内重置）
   7 Caps tap/hold 状态机              ← 单击开关 vim(开=从 Insert 起)/长按临时 Normal/回原模式；Fn+Caps 无特殊
   8 §2.1 快捷键表                     ← 两键盘完全一致(BSPC/Space/-/Shift+=/Ctrl+F/B//)：
                                          base+mods 匹配+kv_cancel 前置+send_plain_tap
   9 vim_glue_engine                   ← 右 Shift 懒发送 + 引擎分发（Insert Esc 直落透传）
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
  } vim_mouse_cfg_t;            /* cfg 同时携带 hook_pre/hook_post_myfn/myfn 分发 */
  bool vim_mouse_process(uint16_t kc, keyrecord_t *r, const vim_mouse_cfg_t *cfg);
  void vim_mouse_release_all(void);
  ```
  > 上面是**鼠标子集**的视图；实际传入共享层的完整结构是 `vim_cfg_t`（`qmk/vim_keymap_common.h`），
  > 它还带 `led_index`（模式色灯位）与 `insert_flash_color`（`Normal--Esc-->Insert` 3s 提示色，见下）。
- **tap/hold 计时 helper**（Caps/鼠标触发键/Fn+Esc 3s 等**一切长按判定共用**，含 timer 防零）；
  > **Esc 宽限窗口用 32 位计时**（`vim_timer_start32()` / `timer_elapsed32()`）：QMK 的 `timer_read()` 是
  > `(uint16_t)timer_read32()`，16 位比较在 **65536ms 处回绕**——已过期的窗口会重新被判为有效
  > （持续打字 65.5s 后出现 3s 假命中）。凡窗口长度可能被"长时间不检查"跨越的计时，一律 32 位。
- **`send_plain_tap(kc)`**："剥修饰发裸键"（§2.1 例外：临时 clear+恢复）；
- **`vim_task(now_ms)`** = `vim_glue_task` + 鼠标长按检查（拖动/长按修饰进入）；
- **`vim_rgb_state_color(enabled, m, pending, mouse, &r,&g,&b)`**：六色计算（绿/蓝/黄/紫/青/红、
  pending 不覆盖 Visual）——spec 级；键盘只提供**灯位索引**（`cfg->led_index`）。
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
CAD 的触发检测+厂商调用）、底排键位与触发键**定义**、VIA、`vim_mouse_cfg_t` 实例。
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
