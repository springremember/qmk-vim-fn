# Caps 长按模块：设计与实现规格

> 面向实现。目标行为见 [`readme.md`](readme.md)；版本与缺陷见 [`changes.md`](changes.md)；用例见 [`testcase.md`](testcase.md)。
> 本文是 Caps 模块的**唯一权威**：实现必须与本文一致，行为改动必须先改本文（见 [`../qmk/README.md`](../qmk/README.md) §2「文档先行」）。

---

## 1. 职责与分层

| 层 | 职责 |
| :--- | :--- |
| 引擎（`engine/`） | **不参与**。Caps 模式与 vim 引擎无关，模式内的键不喂引擎 |
| 共享 keymap 层（`qmk/vim_keymap_common.{c,h}`） | **本模块的唯一实现**：判定进入/退出、键映射、注册/反注册、防卡键 |
| 键盘层（各 keymap） | **零代码接入**：只需 `cfg->fn_layer`（识别 `Fn`+`Caps`）与既有 `Caps` 键位；不得自行复制本模块逻辑（Caps 模式**不再使用** `cfg->hold_ms`——`hold_ms` 仅鼠标模式的长按阈值仍在用） |

## 2. 状态

| 状态 | 含义 |
| :--- | :--- |
| `s_caps_armed` | `Caps` 按下时未按 `Fn`（→ 走 Caps 模式语义）；`false` 表示这是 `Fn+Caps`（release 时开关 vim） |
| `s_caps_touched` | 本次 `Caps` 按下期间是否已按过其它键（仅记录）。**现行实现不读取该值**：撤销与正常退出都调用同一条 `caps_mode_exit()`——未夹键时 held 表为空，两者效果等价 |
| `s_caps_mode` | 模式是否激活 |
| `s_caps_held[]` / `s_caps_held_n` | 本模式**实际注册过**的键码有界表（用于退出时全部反注册；容量固定，表满时新键**既不注册也不发出**，见 §3.1-5） |
| `s_caps_ctrl_n` | 非 F 键按下计数（Ctrl 的引用计数） |
| `s_caps_was_pressed` | `Caps` 是否确实按下过（孤立 release 守卫，§3.1-6） |
| `s_caps_ctrl_owned` | 本模式**确实注册过哪些** Ctrl 键码（LCTL/RCTL 位掩码；只有对应位才反注册） |
| `s_caps_phys_ctrl_held` | 模式内**物理按住**的 Ctrl 键码位（退出时跳过、不反注册它们） |
| `s_caps_phys_ctrl` | 进入模式时物理 Ctrl 是否已被按住（是则本模块不注册/不反注册 Ctrl）。**模式内收到物理 Ctrl 抬起时清为 false**，使后续非 F 键重新自注册 Ctrl |

初始化（`vim_keymap_common_init()`）把上述状态全部清零。

## 3. 判定流程

进入与退出**都在共享层的状态机**里完成，键盘侧无需任何钩子：

```
Caps press  -> 若 Fn 层在按下这一刻已激活：
                 s_caps_armed = false            // Fn+Caps：单击语义（release 时开关 vim）
               否则：
                 s_caps_armed = true；s_caps_touched = false
                 立即进入 Caps 模式（记录物理 Ctrl 状态、清空 held 表与计数）
               消费该 press（配对表）
模式内按键  -> 每按一个键（层键/非基础键码等豁免键除外，见 §3.1-4）置 s_caps_touched = true（见 §4），并正常注册/反注册
Caps release-> 若 !s_caps_armed（Fn+Caps，且本次确有 press，见 §3.1-6）：set_vim_enabled(!vim_enabled())
               否则若 !s_caps_touched：撤销本次进入（反注册 held 表 + Ctrl）
               否则：正常退出（反注册 held 表 + Ctrl）
               // 现行实现把"撤销"与"正常退出"合并为同一次 caps_mode_exit()：未夹键时 held 表为空，结果等价
```

- **不等 `hold_ms`**：进入发生在按下瞬间，因此"按下 Caps 后立刻按 1"必然是 `F1`（而不是 Normal 的计数）。
  代价：若按下期间未按任何其它键就抬起，本模式会短暂进入再撤销——撤销只反注册本模式发出的键，
  不改变 vim 开关/模式；**未撤销**的是"按下期间夹了键"的情况（那些键已按模式映射发出，属预期）。
- **裸 `Caps` 单击无任何效果**（不开关 vim）；**`Fn` + `Caps` 单击 = 开关 vim**（`Fn` 必须先按住）。
- 键盘层若在 `Caps` 按住期间对 `Fn` 等键有既有处理，其顺序不受影响：Caps 模式只接管模式**激活期间**的按键。

## 3.1 进入/退出/重入的清理规则（防止宿主键卡住）

1. **重入先清理**：`Caps` 再次按下且模式**已激活**时（上一次 release 丢失、一次按抬被上报两次、
   或两个物理键都映射 `KC_CAPS`），必须先 `caps_mode_exit()` 反注册上一实例登记过的全部键与 Ctrl，
   再进入新实例；否则旧实例的注册会因 held 表被重置而**永远无法反注册**（宿主键永久卡住）。
2. **release 必须按"本实例是否注册过该键"过滤**：模式内到达的 release，若该键**不在** held 表中
   （其 press 早于本次进入、或属于上一个实例），则**不反注册、也不改 Ctrl 引用计数**，只交配对表消费；
   否则会提前释放仍被其它键需要的 Ctrl。
3. **物理 Ctrl 中途松开要更新 `s_caps_phys_ctrl`**：模式内收到 Ctrl 抬起后清 false，使下一个非 F 键
   按引用计数重新注册 Ctrl——保证「其余键 = Ctrl+键」在整段模式内都成立。
4. **层键豁免且放行**：模式内按下的 QMK 层键（`vim_is_layer_key()` 为真）**不注册任何宿主键**
   （层键的低字节不是键位语义），且**不消费** press——交回 QMK，使 `Fn` 层仍能正常激活。
5. **溢出必须吞吐一致**：held 表有容量上限；表满时**新键既不注册、也不让后续环节看到**
   （两沿都由本模式吞掉，宿主收不到任何键）。**不能透传给 QMK** —— 否则该键会被 §2.1 快捷键表或
   vim 引擎劫持（第 3 轮 K/O2：第 13 键按 `Space` 会发成裸 `→`、按 `d` 会执行 `dd` 删行）。
   绝不出现"注册了但没记表"的键——那是退出时无法反注册的卡键来源。
   **物理 Ctrl 例外**：表满时按下的 `LCTL`/`RCTL`，其 press 被吞（入配对表），但 release 仍按 §4
   的 Ctrl 特例交回 QMK，并 `pair_drop` 掉该配对——否则宿主 Ctrl 位会永久卡住。
   即"两沿都由本模式吞掉"只对非 Ctrl 键成立。
6. **release 守卫**：`Caps` release 只在**本次按下被本层接管过**时才处理（无 press 的孤立 release 无效果）。

## 4. 模式内按键翻译

```
press   base = keycode & 0xFF（QMK 基础键码）
        fkey = map(base)                      // 1..0 - = -> F1..F12，其余 KC_NO
        fkey != KC_NO : register(fkey)        // 不带 Ctrl；Ctrl 计数不变
        否则           : 若 (!phys_ctrl && !ctrl_owned) -> register(Ctrl); ctrl_owned = true
                         ctrl_n++
                         register(base)        // 含修饰键：Shift -> Ctrl+Shift
        消费该 press（配对表）
Ctrl 键（LCTL/RCTL）特例：**press 与 release 两沿都交回 QMK**（本层不消费、不入配对表），
        只更新 phys_ctrl / phys_ctrl_held 基线。理由：真机修饰键是位图，若本层吞掉它的 release，
        QMK 永远收不到释放 -> 位永久卡住（第 4 轮 P0-1）。
        （唯一例外：held 表已满时其 press 先被 §3.1-5 溢出规则吞掉并入配对表，release 再 `pair_drop`。）
否则 sent = fkey != KC_NO ? fkey : base
        若该键不在 held 表（press 早于进入 / 属上一实例）: 交配对表消费，结束
        unregister(sent)
        若为 F 键：无 Ctrl 动作
        否则      : ctrl_n--
                     若 ctrl_n == 0 且 ctrl_owned 的对应位为真 且该位未被物理 Ctrl 占用
                       -> unregister(该位 Ctrl); 清除 ctrl_owned 对应位
```

> **注意**：Ctrl 的注册条件是 `(!phys_ctrl && !ctrl_owned)` 而**不是** `ctrl_n == 1`——这样"物理 Ctrl
> 中途松开、但仍有非 F 键按住"时，下一个非 F 键会重新注册 Ctrl（第 2 轮 P0-2）。反注册的条件是
> `ctrl_n == 0 && ctrl_owned` 的对应位为真 **且该位当前未被物理 Ctrl 占用**（`s_caps_phys_ctrl_held`，
> **逐位**判断）——只按 `ctrl_n == 0 && ctrl_owned` 会在"先合成 Ctrl、再物理按住同侧 Ctrl"时把物理位
> 一起清掉（第 4 轮 P1-2、第 7 轮 P2-1；与 §2 `s_caps_phys_ctrl_held`、§4.3 不变量 3 一致）。

**不变量**

1. **F 区不带 Ctrl**：F 键路径完全不碰 Ctrl（既不注册也不因它改变引用计数）。
2. **修饰键对称**：模式内注册过的修饰键，在它自己的 release 上原样反注册（不会把物理按住卸掉）。
3. **退出必清**：退出模式时，`s_caps_held[]` 中记录的所有键一律反注册（即使其 release 尚未到达）；
   Ctrl 仅当 `s_caps_ctrl_owned` 的对应位为真（本模式确实注册过）**且该位未被物理 Ctrl 占用**
   （`s_caps_phys_ctrl_held`，逐位判断，见 §2）才反注册——否则会卸掉物理按住的 Ctrl。
4. **不碰 vim**：模式期间不调用引擎、不改变 vim 开关/模式、不重置 pending（模式内的键根本没进引擎）。
5. **边沿配对**：模式内被本层消费的 press，其 release 由共享配对表无条件消费（不会漏出孤立 release）。

## 5. 与 pipeline 的关系

Caps 模式拦截位于共享 keymap 层拦截链的**最前**（`Caps` tap/hold 判定本身也在其中）：

| 步骤 | 内容 | 模式内 |
| :--- | :--- | :--- |
| 0 | 物理修饰键影子更新 | 仍执行（影子必须准确） |
| 1 | `Caps` tap/hold（进入/退出判定） | 仍执行（用于退出） |
| 2 | **Caps 模式拦截** | **在模式内接管一切按键** |
| 3… | myfn 骨架 / 键盘钩子 / 鼠标 / Shift+Esc / Esc 切换 / 快捷键表 / 引擎 | 模式内**不可达** |

因此：模式内 `Esc` = `Ctrl+Esc`（不触发 vim 的 Esc 切换）；模式内 `Fn` 不进入 myfn 分发。

## 6. 可测性与覆盖

- **主机测试可覆盖**：进入时机（按下即入，无 `hold_ms` 边界）、F 区映射、Ctrl 引用计数、修饰键、
  重叠按键、退出/撤销防卡键、**重入清理**、**过期 release 过滤**、**物理 Ctrl 中途松开**、
  **层键豁免**、**held 表溢出**、vim 状态不变、vim 关闭可用、与 pipeline 的先后关系。
- **只能实机验证**：宿主观感（`Ctrl+字母` 的实际效果、`Ctrl+Shift+字母` 是否被编辑器当组合键）、
  以及与本键盘厂商功能（Fn 层、无线切换等）同时使用时的真实行为。
