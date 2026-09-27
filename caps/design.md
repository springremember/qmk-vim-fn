# Caps 长按模块：设计与实现规格

> 面向实现。目标行为见 [`readme.md`](readme.md)；版本与缺陷见 [`changes.md`](changes.md)；用例见 [`testcase.md`](testcase.md)。
> 本文是 Caps 模块的**唯一权威**：实现必须与本文一致，行为改动必须先改本文（见 [`../qmk/README.md`](../qmk/README.md) §2「文档先行」）。

---

## 1. 职责与分层

| 层 | 职责 |
| :--- | :--- |
| 引擎（`engine/`） | **不参与**。Caps 模式与 vim 引擎无关，模式内的键不喂引擎 |
| 共享 keymap 层（`qmk/vim_keymap_common.{c,h}`） | **本模块的唯一实现**：判定进入/退出、键映射、注册/反注册、防卡键 |
| 键盘层（各 keymap） | **零代码接入**：只提供 `cfg->hold_ms`（进入阈值）与既有 `Caps` 键位；不得自行复制本模块逻辑 |

## 2. 状态

| 状态 | 含义 |
| :--- | :--- |
| `s_caps_armed` | `Caps` 按下时未按 `Fn`（→ 走 Caps 模式语义）；`false` 表示这是 `Fn+Caps`（release 时开关 vim） |
| `s_caps_touched` | 本次 `Caps` 按下期间是否已按过其它键（决定快速抬起时是否撤销） |
| `s_caps_mode` | 模式是否激活 |
| `s_caps_held[]` / `s_caps_held_n` | 本模式**实际注册过**的键码有界表（用于退出时全部反注册；容量固定，溢出时该键仍会发出，只是退出时不保证被强制释放） |
| `s_caps_ctrl_n` | 非 F 键按下计数（Ctrl 的引用计数） |
| `s_caps_phys_ctrl` | 进入模式时物理 Ctrl 是否已被按住（是则本模块不注册/不反注册 Ctrl） |

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
模式内按键  -> 每按一个键置 s_caps_touched = true（见 §4），并正常注册/反注册
Caps release-> 若 !s_caps_armed（Fn+Caps）：set_vim_enabled(!vim_enabled())  // 单击开关 vim
               否则若 !s_caps_touched：撤销本次进入（反注册 held 表 + Ctrl）
               否则：正常退出（反注册 held 表 + Ctrl）
```

- **不等 `hold_ms`**：进入发生在按下瞬间，因此"按下 Caps 后立刻按 1"必然是 `F1`（而不是 Normal 的计数）。
  代价：若按下期间未按任何其它键就抬起，本模式会短暂进入再撤销——撤销只反注册本模式发出的键，
  不改变 vim 开关/模式；**未撤销**的是"按下期间夹了键"的情况（那些键已按模式映射发出，属预期）。
- **裸 `Caps` 单击无任何效果**（不开关 vim）；**`Fn` + `Caps` 单击 = 开关 vim**（`Fn` 必须先按住）。
- 键盘层若在 `Caps` 按住期间对 `Fn` 等键有既有处理，其顺序不受影响：Caps 模式只接管模式**激活期间**的按键。

## 4. 模式内按键翻译

```
press   base = keycode & 0xFF（QMK 基础键码）
        fkey = map(base)                      // 1..0 - = -> F1..F12，其余 KC_NO
        fkey != KC_NO : register(fkey)        // 不带 Ctrl；Ctrl 计数不变
        否则           : ctrl_n++；若 ctrl_n == 1 且无物理 Ctrl -> register(Ctrl)
                        register(base)        // 含修饰键：Shift -> Ctrl+Shift
        消费该 press（配对表）
release sent = fkey != KC_NO ? fkey : base
        unregister(sent)
        若为 F 键：无 Ctrl 动作
        否则      : ctrl_n--；若 ctrl_n == 0 且无物理 Ctrl -> unregister(Ctrl)
```

**不变量**

1. **F 区不带 Ctrl**：F 键路径完全不碰 Ctrl（既不注册也不因它改变引用计数）。
2. **修饰键对称**：模式内注册过的修饰键，在它自己的 release 上原样反注册（不会把物理按住卸掉）。
3. **退出必清**：退出模式时，`s_caps_held[]` 中记录的所有键与 Ctrl 一律反注册，即使其 release 尚未到达。
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

- **主机测试可覆盖**：进入/退出时机、`hold_ms` 边界、F 区映射、Ctrl 引用计数、修饰键、重叠按键、
  退出防卡键、vim 状态不变、vim 关闭可用、与 pipeline 的先后关系。
- **只能实机验证**：宿主观感（`Ctrl+字母` 的实际效果、`Ctrl+Shift+字母` 是否被编辑器当组合键）、
  以及与本键盘厂商功能（Fn 层、无线切换等）同时使用时的真实行为。
