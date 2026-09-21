# vim 引擎：技术架构与实现细节

> 面向实现。描述 vim 引擎的**目标架构**（按模式解耦 + token 队列 + 表驱动 + pending 严格清空），
> 借鉴 Vim 源码的语法模型，并针对固件"只能发键序列、不能改缓冲区"的现实设计。
> 使用说明见 [`readme.md`](readme.md)；变更与历史问题见 [`changes.md`](changes.md)；
> 测试见 [`testcase.md`](testcase.md)。
>
> 注：本文件为重构设计的**唯一权威**，取代 `qmk-vim/docs/REDESIGN_PLAN.md`（历史草案，勿引）。

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

### 4.6 架构与文件清单（`engine/`，QMK 无关）
```
engine/
  include/kv.h          // 公共 API：kv_kbd / kv_set_emit / kv_task + 查询/设置（见 §4.7）；类型、模式、keycode
  include/kv_kc.h       // kv_keycode_t 与修饰位（镜像 QMK 16-bit 布局，便于接回）
  src/queue.{h,c}       // 环形队列：push / pop / peek / flush
  src/classify.{h,c}    // keycode -> token 类别
  src/ctx.{h,c}         // kv_ctx：count / op / 前缀 / nchar 累积与重置
  src/emit.{h,c}        // 命令/区间 -> 固定宿主键序列 + 非阻塞发送队列
  src/repeat.{h,c}      // 命令 token 记录；'.' 回放
  src/engine.c          // feed() 解析循环；严格清空；kv_task() 排空发送队列；模式调度
  src/modes/modes.h     // 模式表接口：每模式 kv_rule_t[] + enter/exit
  src/modes/normal.c
  src/modes/insert.c
  src/modes/visual.c
  test/                 // 主机单测：喂 token -> 捕获 emit -> 断言
  Makefile              // 仅主机测试；不参与 QMK 构建
```

### 4.7 关键接口
```c
/* 解析器入口：只喂 key-down；key-up 由胶水层一律透传（held motion 例外） */
void kv_kbd(kv_keycode_t kc);

/* 输出回调：引擎把宿主键序列交给它；单测里换成记录器 */
typedef void (*kv_emit_fn)(kv_keycode_t kc);
void kv_set_emit(kv_emit_fn fn);

/* 由 housekeeping 调用：按计时发送 emit 队列（替代阻塞的 wait_ms） */
void kv_task(uint32_t now_ms);

/* ---- 查询/设置接口（供键盘层：Caps 恢复、RGB 指示、Fn+Caps 开关、前置分支取消）---- */
kv_mode_t kv_get_mode(void);        /* 当前模式（含 Visual/Visual-Line） */
bool      kv_vim_enabled(void);     /* vim 总开关 */
bool      kv_pending(void);         /* 是否有 pending（计数/操作符/前缀/缩进） */
void      kv_set_mode(kv_mode_t m); /* 直接设模式（如 Caps 恢复进入前模式） */
void      kv_enable(void);          /* 开 vim */
void      kv_disable(void);         /* 关 vim（RGB 红） */
void      kv_cancel(void);          /* 取消当前 pending（不发键），供 keymap 前置分支 */
```

> 上述查询/设置接口为**需新增**（键盘迁移计划依赖，见 `键盘迁移计划.md` §0/§2.1）；
> 其中 `kv_cancel()` 在 pending 态等价于喂入 `Esc`（仅清 pending，不 emit；Idle 态则无操作）。

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
  `i/I/a/A/o/O`→INSERT；`v/V`→VISUAL；`Esc`→透传。
  - 变更类 `s/C/S/c`：进入 Insert（`c` 为操作符，其"改"结果同样进入 Insert）。
- **OP_PENDING（瞬态）**：移动设区间→emit；非期望键→清空+重新识别；Esc→取消。
- **INSERT**：普通字符透传；`Esc`=真 Esc 发宿主（不切模式）；离开 Insert 靠 `Caps`。
- **VISUAL / VISUAL_LINE**：`v/V` 选区，移动扩展；`d/y/c/x/s/p` 复用 Normal 命令表；未列键（如 `i`/`a`）为非法键 → **留在 Visual**（吞键，不退出、不插入）；`Esc`→退出选区。
- **MOUSE**：键盘层。**右 Alt 短按**（阈值 **200ms**，与 Caps 一致）在 `Insert`/`Normal`/`Visual` 均可进/出（长按=RAlt 修饰）；模式内 `hjkl`=指针、`Shift+J`/`Shift+K`=滚轮下/上、`Space`=左键（短按单击/长按拖动）、`Enter`=右键、其它键退出并重新识别；RGB 指示为**青**（详见 [`readme.md`](readme.md) §8）。

### 4.10 修饰键、key-up 与输入保真
- **key-up 一律透传**；唯一例外是按住连发移动 `h/j/k/l`（down `register` 宿主方向键 / up `unregister`）。
- **keymap 层消费 press 的键，其 release 也须一并消费**：keymap 前置分支（如 `Shift+Esc` 组合）在按下时消费了某键，必须记住并**无条件吞掉其抬起**，否则 release 会落到普通路径而多打出一个键。这不违反上一条——上一条管**引擎**侧，keymap 自消费的键由 keymap 自己负责抬起。
- **非 vim 键码一律透传**。
- **修饰键影子**：胶水层用物理修饰键影子判断/拼序列，**不打包、不 `clear_mods`/`set_mods`**。
- **emit 非阻塞**：`kv_task()` 按计时发送；不使用 `wait_ms`。

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
