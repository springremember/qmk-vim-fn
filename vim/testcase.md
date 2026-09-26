# vim 引擎测试用例

> 主机侧单测：喂 token 序列 → 捕获 emit 序列/状态 → 断言。
> "关联"列指向 [`changes.md`](changes.md) 的问题编号（`E*`/`A*`）或 [`design.md`](design.md) 的决策号（`#N`）；空白为覆盖性用例。
> 鼠标模式为**键盘层行为**（见 [`readme.md`](readme.md) §8），由 glue 主机单测覆盖（`engine/test/glue/`），不在 engine 纯核心单测范围（如需可另做 e2e 冒烟）。
> release-swallow 通用规则（[`design.md`](design.md) §4.10）属 **keymap 层**行为（如 `Shift+Esc`），同样由 glue 主机单测覆盖。

## 1. 单键
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 左移 | `h` | `←` | |
| 上移 | `k` | `↑` | |
| 右移 | `l` | `→` | |
| 下词首 | `w` | `Ctrl+→` | |
| 上词首 | `b` | `Ctrl+←` | |
| 下词尾 | `e` | `Ctrl+→` | |
| 行首 | `0` | `Home` | |
| 行首(^) | `^` | `Home` | |
| 行尾 | `$` | `End` | |
| 文档末 | `G` | `Ctrl+End` | |
| 删字符 | `x` | `Delete` | |
| 前一字符 | `X` | `Backspace` | |
| 改字符 | `s` | `Shift+→`,change(+Insert) | |
| 撤到行尾 | `C` | 选到行尾→`Ctrl+X`(+Insert) | |
| 删到行尾 | `D` | 选到行尾→`Ctrl+X` | |
| 复制到行尾 | `Y` | 选到行尾→`Ctrl+C` | |
| 整行改 | `S` | `Home`,`Home`,`Shift+End`,change(+Insert) | |
| 粘贴（字符） | `p` | `Ctrl+V` | |
| 粘贴（行） | `p`（`yanked_line`） | `End`,`→`,`Ctrl+V` | |
| 向前粘（字符） | `P`（非行） | `←`,`Ctrl+V` | |
| 向前粘（行） | `P`（`yanked_line`） | `End`,`→`,`↑`,`Ctrl+V` | |
| 合并 | `J` | `End`,`Delete` | |
| 撤销 | `u` | `Ctrl+Z`（**单次**） | E4 |
| 重复 | `.` | 重放上一命令 token | A2 |
| Normal Esc 透传 | `Esc`（Normal） | 发真实 `Esc`，回 Insert | |
| replace 透传 | `R` / `Shift+R` | 原样透传（不进入 replace 模式） | |

## 2. 操作符 + 移动
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 删词 | `dw` | 选词→`Ctrl+X` | |
| 删到行尾 | `d$` | 选到行尾→`Ctrl+X` | |
| 删到行首 | `d0` | 选到行首→`Ctrl+X` | |
| 删到文末 | `dG` | 选到文末→`Ctrl+X` | |
| 改词 | `cw` | 选词→`Ctrl+X`(+Insert) | |
| 复制词 | `ye` | 选词→`Ctrl+C` | |
| 操作符+`0`（丢计数） | `2d0` | 删到行首（丢弃 2） | #2b |
| 操作符计数 | `d3w` | 删除 3 个词 | #2 |
| 双计数相乘 | `2d3w` | 等价 `d6w`（6 词） | #2 |
| 计数跨操作符折叠 | `3dw` | 内部当 `d3w`（删除 3 词） | #2 |
| 计数 G（丢弃） | `42G` | `Ctrl+End`（丢弃 42） | #2b |

## 3. 行操作（自叠）
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 删行 | `dd` | `Home,Home,Shift+End,Ctrl+X,Backspace` | E1,E4 |
| 删 3 行 | `3dd` | `Home,Home,Shift+End,Shift+Down×2,Ctrl+X,Backspace`（单次选区覆盖 3 行） | |
| 复制行 | `yy` | `Home,Home,Shift+Down×1,Ctrl+C` | |
| 改行 | `cc` | `Home,Home,Shift+End`,change(+Insert) | |
| 改 3 行 | `3cc` / `3S` | `Home,Home,Shift+End,Shift+Down×2`,change(+Insert)（单次选区覆盖 3 行） | |
| 复制 3 行 | `3yy` | `Home,Home,Shift+Down×3,Ctrl+C`（单次选区覆盖 3 行） | |
| `dd` 末行 | 在文档末行 `dd` | 可删除 | E1 |
| `dd` 首行 | 在首行 `dd` | 允许留一个空行（已知取舍） | E1 |
| `dd` 后撤销 | `dd`,`u` | **只恢复一半**，需再 `u` | E4,#10 |

## 4. 缩进
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 缩进当前行 | `>>` | 缩进 | |
| 反缩进当前行 | `<<` | 反缩进 | |
| 缩进到行首 | `>0` | 缩进到行首（`0` 作行首、丢弃 n） | |
| 反缩进到行首 | `<0` | 反缩进到行首 | |
| 缩进到移动 | `>j` | 缩进到下一行 | |
| 计数缩进 | `3>>` | 缩进 3 行 | |
| 操作符+计数 | `2>3j` | 缩进 6 行 | #2 |
| 后置计数含 0 | `>10j` / `>20j` | 缩进 10 / 20 行（`0` 续接计数） | #2 |

## 5. 前缀 `g` / `Z`
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 文档首 | `gg` | `Ctrl+Home` | |
| 操作符+gg | `dgg` | 删到文档首 | |
| 保存 | `ZZ` | `Ctrl+S` | |
| g 后非 g（vim 键） | `g` `x` | 清空 g，执行 `x`（删字符） | |
| g 后非 g（非 vim 键） | `g` `F5` | 清空 g，原样发 `F5` | |
| Z 后非 Z | `Z` `x` | 清空 Z，执行 `x` | |

## 6. 计数
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 前缀计数多位移移动 | `12w` | 移动 12 词 | |
| 计数不作用于 `x` | `3x` | 丢弃 3，`x` 执行一次 | A1 |
| 计数不作用于 `s` | `3s` | 丢弃 3，`s` 执行一次 | A1 |
| 计数不作用于 `p` | `3p` | 丢弃 3，`p` 执行一次 | A1 |
| 计数不作用于 `P` | `3P` | 丢弃 3，`P` 执行一次 | A1 |
| 计数不作用于 `J` | `3J` | 丢弃 3，`J` 执行一次 | A1 |
| 计数不作用于 `u` | `3u` | 丢弃 3，`u` 执行一次 | A1 |
| 计数不作用于 `.` | `3.` | 丢弃 3，`.` 执行一次 | A1 |
| 计数不作用于 `gg` | `3gg` | 丢弃 3，`gg` 执行 | #2b |
| 计数不作用于 `ZZ` | `3ZZ` | 丢弃 3，`ZZ` 执行 | A1 |
| 计数不作用于 `X` | `3X` | 丢弃 3，`X` 执行一次 | A1 |
| 计数不作用于 `C/D/Y` | `3C`/`3D`/`3Y` | 丢弃 3，各执行一次 | A1 |
| `S` 接受计数 | `3S` | 改 3 行（≡`3cc`） | #2b |
| 计数不作用于插入键 | `3i` / `3I` / `3a` / `3A` / `3o` / `3O` | 丢弃 3，进入 Insert | A1 |
| 计数不作用于 `v`/`V` | `3v` / `3V` | 丢弃 3，进入 Visual / Visual-Line | A1 |
| 计数上限 2 位 | `99w` / `123w` | `99w` 有效；`123w`≡`12w` | A7 |
| 首位非 0 | `0w` | `0`=行首，然后 `w` | |
| 操作符内 `G` 丢计数 | `2dG` / `d2G` | 都 ≡`dG` | #2b |
| 操作符内 `gg` 丢计数 | `2dgg` / `d2gg` | 都 ≡`dgg` | #2b |
| 缩进内 `G` 丢计数 | `>G` / `2>G` / `>2G` | 都 ≡`>G`（缩进到文档末，保留 `>`） | #2b |
| 缩进内 `gg` 保留 | `>gg` | 从当前行缩进到文档首（保留 `>`） | #2b |
| 缩进内 `gg` 丢计数 | `2>gg` / `>2gg` | 都 ≡`>gg` | #2b |
| 行缩进计数 | `3<<` | 反缩进 3 行 | |
| 悬空计数等待 | `dw3` | `dw` 执行；`3` 进入 `Cnt` 等待下一键 | |
| 悬空计数后套用 | `dw3w` | `dw` 后 `3w` | |

## 7. Visual
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 进入并删 | `v` `l` `d` | 扩展选 1 字符→删除 | |
| 选择复制 | `v` `e` `y` | 扩展选区→复制 | |
| 整行选择 | `V` `j` `d` | 按行扩展→删除 | |
| 退出 | `v` `Esc` | 退出选区回 Normal | |

## 8. Insert
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 进入 | `i` | 原地 Insert | |
| 行首插入 | `I` | `Home` → Insert | |
| 追加 | `a` | `→` → Insert | |
| 行尾插入 | `A` | `End` → Insert | |
| 下方开行 | `o` | `End`,`Shift+Enter` → Insert | |
| 上方开行 | `O` | `Home`,`Shift+Enter`,`↑` → Insert | |
| Esc 进入 Normal | Insert 下 `Esc`（非宽限） | 吞键，转入 Normal，不发 Esc | |
| Esc 宽限内 | Normal->Insert Esc 后 3s 内 `Esc` | 发真实 Esc，留 Insert，重置 3s | |
| 普通字符 | Insert 下 `a` | 透传字符 | |
| 回到打字提示色：无窗口 | 开机（Insert，未按过 Esc） | `vim_insert_flash()`=false | §10 |
| 回到打字提示色：开窗 | Normal 空闲 `Esc` 回 Insert | `vim_insert_flash()`=true，`vim_insert_flash_color()`=true 并给出 `cfg.insert_flash_color` | §10 |
| 回到打字提示色：3s 边界 | 开窗后 2999ms / 恰好 3000ms | 前者 true；后者 false，回 Insert 绿 | §10 |
| 回到打字提示色：窗口内 Esc 续期 | 开窗 → +2999ms `Esc` → 再 +2999ms | 仍 true（重置计时，非从首次开窗算） | §10 |
| 回到打字提示色：其它入口 | 引擎 `i`/`a`/`o`/`I`/`A`/`O` 进入 Insert | false | §10 |
| 回到打字提示色：离开 Insert | 开窗后回 Normal / Visual / 鼠标模式 | false | §10 |
| 回到打字提示色：vim 关 | `Caps` 关 vim | false（模式色红） | §10 |
| 回到打字提示色：色值 0 | `cfg.insert_flash_color = 0` | `vim_insert_flash_color()`=false（不覆盖） | §10 |
| 回到打字提示色：16 位回绕 | 开窗后时间推进 65536ms（不按任何键） | false（窗口**不得**因 `uint16` 回绕复活） | §10 |
| 回到打字提示色：32 位计时 | 开窗 → +65536ms → +2999ms / +3000ms | 前者 false；后者仍 false（窗口只按首次开窗算 3s） | §10 |
| 宽限窗口回绕时不误吞 | 回绕后（窗口已过期）Insert 下 `Esc` | 吞键进 Normal（不得因回绕变成真实 Esc） | §10 |

## 9. pending 严格清空
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 操作符+非期望(vim键) | `d` `x` | 清空 `d`，`x` 删字符 | A4 |
| 操作符+非期望(非 vim) | `d` `F5` | 清空 `d`，原样发 `F5` | |
| Esc 取消 | `d` `Esc` | 清空，无副作用 | |
| 计数中非期望(vim键) | `3` `x` | 清空计数，`x` 执行 | A1 |
| g 前缀不吞键 | `g` `F6` | 发 `F6` | |
| 无超时 | `d`（久置后再按键） | 仍 pending，等下一键决定 | |
| **模式切换清空（API）** | `2d` → `kv_set_mode(INSERT)` → `kv_set_mode(NORMAL)` → `w` | `w` 只执行移动（不残留 `dw`）；`kv_pending()==false` | §4.7 |
| **使能切换清空（API）** | `2d` → `kv_disable()` | `kv_pending()==false`；`kv_kbd(任意)` 全 `KV_PASSTHROUGH` | §4.7 |
| **重新使能起点** | 任意模式 → `kv_disable()` → `kv_enable()` | `kv_get_mode()==INSERT` | §4.7 |
| **repeat 不跨模式污染** | `2d` → `kv_set_mode(INSERT)` → `kv_set_mode(NORMAL)` → `w` → `.` | `.` 回放 `w`（不得回放 `2dw`） | §4.7 |
| **repeat 跨模式保留** | `dd` → `kv_set_mode(INSERT)` → `kv_set_mode(NORMAL)` → `.` | `.` 回放 `dd` | §4.7 |
| **Shift+Esc（Visual 内）** | `v` `LSFT+Esc` | 退出 Visual 回 Normal（不吞死） | §4.10 |

## 10. 修饰键 / key-up / held motion
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 幽灵修饰键不出现 | 任何命令 | 不打包、不回写 `set_mods`，无幽灵位 | E2 |
| Alt+Tab 不卡 | `Alt+Tab` press 透传、release 透传 | Tab 不卡 | E3 |
| key-up 透传 | 除 held motion `h/j/k/l` 外的任意键 release | 引擎不消费释放 | E3 |
| 按住移动 | `h` 按住 | 连续左移，松开停止 | |
| 松开不卡方向键 | 松开 `h` | 宿主 `←` 被释放 | |
| held motion + 修饰键（仅 hjkl） | Normal `Win`+`h` | `Win+←` 方向键（hold），不把裸 `h` 透传 | |
| held motion + 修饰键（前缀不例外） | `d` `Ctrl`+`h` | 严格清空 `d`，`Ctrl+h` 透传 | |
| Caps 单击 | `Caps` 单击 | 切换 vim 开/关（开=Insert 起） | |
| Caps 长按 | `Caps` 长按 ≥200ms | 临时 Normal，松手回原模式 | |
| Esc 三态 | Insert 宽限内 / 宽限外、Normal 空闲 | 见 §8 | |
| 右Shift 单独 | 右Shift 按/松 | 无输出（不注册 Shift） | |
| 右Shift+字母 | Insert 下 `右Shift`+`a` | 瞬时 `Shift+a`（=A），无孤立 Shift | |
| 右Shift+修饰 | `右Shift`+`Ctrl`+`C` | `Ctrl+Shift+C` | |
| 右Shift 关闭 vim | vim off 下 `右Shift` | 普通右 Shift | |

## 11. emit 非阻塞
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 无 `wait_ms` 阻塞 | 任意命令 | 命令不阻塞主循环 | #7 |
| 队列按计时发送 | 多段 emit | 由 `kv_task` 逐拍发送 | #7 |

## 12. 回归：V1.0 问题（E1–E6）

> 本表「编号」即关联号（对应 [`changes.md`](changes.md)），「用例」含关键输入/场景。

| 编号 | 用例 | 期望 |
|---|---|---|
| E1 | `dd` 末行 / 首行 | 末行可删；首行留空行（取舍） |
| E2 | 修饰键释放被吞后继续命令 | 不再反复重装修饰位、不卡 `Shift` |
| E3 | `Alt+Tab` | Tab 不卡（key-up 对称透传） |
| E4 | `dd` 后 `u` | 方案 A：一次只恢复一半 |
| E5 | 子模块 bump | 重编并校验 `output`/`.build` 哈希（流程项） |
| E6 | 集成（层数/rgbrec） | 键盘侧，不回归 |

## 13. 回归：状态机隐患（A1–A8）

> 本表「编号」即关联号（对应 [`changes.md`](changes.md)），「用例」含关键输入/场景。

| 编号 | 用例 | 期望 |
|---|---|---|
| A1 | `3x` 后 `j` | 不跳 3 行（计数不泄漏） |
| A2 | `dd`/`dw` 后 `.` | 完整回放，不卡 operator-pending |
| A3 | NKRO 下按住移动 | motion 不卡自动重复 |
| A4 | Insert/Visual 下按方向键 | 不误取消、不误回 Normal |
| A5 | Visual 下 `i`（文本对象入口） | 文本对象已剔除；`i` 作非法键 → 留在 Visual（吞键） |
| A6 | 左右修饰符混按 | 键码正确、不破坏 |
| A7 | 计数上限 | 不溢出、不触发看门狗 |
| A8 | 直接映射的模键码 | 不再打包；修饰位由物理影子提供，键码自带修饰位不被剥离 |
