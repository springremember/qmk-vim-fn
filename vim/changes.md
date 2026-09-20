# 变更记录 — vim 引擎

> 记录 vim 引擎（qmk-vim fork）的变更、历史问题与最终取舍。
> 目标设计见 [`design.md`](design.md)；对应测试见 [`testcase.md`](testcase.md)；使用见 [`readme.md`](readme.md)。

## 1. 版本

- **V1.0（冻结）**：引擎代码 commit `62bb338`；固件另存 `output/*_v1.0.*`，避免被后续重构覆盖。
- 重构目标：见 [`design.md`](design.md)（按模式解耦 + token 队列 + 表驱动 + pending 严格清空）。

---

## 2. 相对上游的功能修改

- `gg` / `G` 改为 `Ctrl+Home` / `Ctrl+End`（比上游的 `Ctrl+A`+方向更可靠）。
- `o` / `O` 换行改用 **`Shift+Enter`**（部分编辑器 `Enter` 行为不一致）。
- `dd` 行删除重写（见 §5 E1）：`Home×2 → Shift+End → Ctrl+X → Backspace`。
- `yy` 行复制：`Home×2 → Shift+Down×n → Ctrl+C`（`Home×2` 抵消 smart-home）。
- 操作符待定遇非法键：**清空 pending 并把该键重新识别**（非 vim 键透传）——取代上游的"中止并吞掉"。
- 计数：仅前缀、**前后相乘**（`2d3w`=`d6w`）；最多 **2 位（≤99）**，第 3 位起忽略；作用域限移动/缩进/行操作（含 `cc`/`S`），其余键（含 `G`/`gg`）**丢弃计数**（见 §3 A1）。
- **丢弃计数的键**：`G`/`gg`（移动例外，任何上下文）、`C D Y X`、`x s p P J u .`、`v V`、插入键、`ZZ`——计数被吸收但不生效。`S`≡`cc` 接受计数。
- Visual-Line 首次 `j` 用 `Home` 折叠，避免在 VSCode 中 `Left` 跨行丢行。
- 插入模式不再无条件 `clear_keyboard()`（见 §3 A3 的目标）。

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
| A8 | 直接映射的模键码修饰位 | 不再打包，模键位由物理影子处理 |

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
