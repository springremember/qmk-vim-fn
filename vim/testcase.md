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
| 删字符 | `x` | `Shift+→`,`Ctrl+X`（选中光标下 1 字符并剪切 → 写无名寄存器） | D7 |
| 前一字符 | `X` | `Shift+←`,`Ctrl+C`,`Backspace`（**不**用 `Shift+←,Ctrl+X`：列 0 空选区会被当成"剪切整行"） | D7 |
| 改字符 | `s` | `Shift+→`,`Ctrl+X`,Insert（`s`≡`cl`） | D7 |
| 撤到行尾 | `C` | 选到行尾→`Ctrl+X`(+Insert) | |
| 删到行尾 | `D` | 选到行尾→`Ctrl+X` | |
| 复制到行尾 | `Y` | **≡ `yy`（行级）**：`Home`,`Home`,`Shift+Down×1`,`Ctrl+C`,`Esc`,`Up×1` | D8 |
| 整行改 | `S` | **≡ `cc`**：`Home`,`Home`,`Shift+End`,`Shift+→`,`Ctrl+X`,`Shift+Enter`,`←`(+Insert) | |
| 粘贴 | `p` | **按无名寄存器类型定位**（`s_reg_linewise`）：字符级=`→`,`Ctrl+V`；行级=`End`,`→`,`Ctrl+V` | D7 |
| 向前粘 | `P` | 同上定位：字符级=`Ctrl+V`；行级=`Ctrl+V`（**不发 `←`**） | D7 |
| 合并 | `J` | `End`,`Space`,`Delete`,`←`（插一个空格并把光标留在空格上；不去前导空白） | D15 |
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
| 删行 | `dd` | `Home,Home,Shift+End,`**`Shift+→`**`,Ctrl+X`（`Shift+→` 把**行尾换行**纳入半开选区，否则首行留空行） | E1 |
| 删 3 行 | `3dd` | `Home,Home,Shift+End,Shift+→,Shift+Down×2,Ctrl+X`（单次选区覆盖 3 行） | |
| 复制行 | `yy` | `Home,Home,Shift+Down×1,Ctrl+C,Esc,Up×1`（`Esc` 取消宿主残留选区；`Up` 回原位） | |
| 改行 | `cc` | `Home,Home,Shift+End,Shift+→,Ctrl+X,Shift+Enter,←`(+Insert) —— **不发 `Backspace`**，留一个空行且寄存器为**行级** | |
| 改 3 行 | `3cc` / `3S` | `Home,Home,Shift+End,Shift+→,Shift+Down×2,Ctrl+X,Shift+Enter,←`(+Insert)（单次选区覆盖 3 行；留一个空行） | |
| 复制 3 行 | `3yy` | `Home,Home,Shift+Down×3,Ctrl+C,Esc,Up×3`（单次选区覆盖 3 行） | |
| `dd` 末行 | 在文档末行 `dd` | 可删除（缓冲区与寄存器均与 Vim 一致） | E1 |
| `dd` 首行 | 在首行 `dd` | **不留空行**（`Shift+→` 已把行尾换行纳入选区；实测 `dd`@L1 与 Vim 一致） | E1 |
| `dd` 后撤销 | `dd`,`u` | **一次 `u` 完整恢复**（`dd` 现为单次 `Ctrl+X`，只有一个宿主编辑步骤；实测 `ddu` 与 Vim 一致） | E4 |

## 4. 缩进
| 用例 | 输入 | 期望 | 关联 |
|---|---|---|---|
| 缩进当前行 | `>>` | `Home`,`Tab`（n=1 无选区，在行首插一个 Tab） | |
| 反缩进当前行 | `<<` | `Home`,`Shift+Tab` | |
| 缩进到行首 | `>0` | 缩进当前行（`0` 作行首、丢弃 n；`h`/`l`/`^`/`$` 同理恒为当前行） | D4 |
| 反缩进到行首 | `<0` | 反缩进当前行 | D4 |
| 缩进到移动 | `>j` | **2 行**（当前行 + 下 1 行 = `n+1` 行；vim.tiny 实测 `>j` 缩进 2 行） | |
| 计数缩进 | `3>>` | 缩进 3 行（n≥2 走多行骨架 + `Esc,Up×n,Home[,Right]`） | |
| 操作符+计数 | `2>3j` | **7 行**（`fold_counts(2,3)=6` ⇒ `>6j` ⇒ `n+1=7`；vim.tiny 实测 7 行） | #2 |
| 后置计数含 0 | `>10j` / `>20j` | **11 / 21 行**（`0` 续接计数；同样是 `n+1` 行，vim.tiny 实测 11 行） | #2 |

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
| 计数作用于 `x` | `3x` | **一次**选中 3 个字符再剪切（`Shift+→×3, Ctrl+X`）—— 寄存器拿到全部 3 个（逐个删只剩最后一个） | A1,P0-6 |
| 计数作用于 `s` | `3s` | `Shift+→×3, Ctrl+X`,Insert（寄存器拿到全部 3 个） | A1 |
| 计数作用于 `p` | `3p` | **只定位一次**再 `Ctrl+V×3`（逐个定位会把副本交错插入） | A1,P0-5 |
| 计数作用于 `P` | `3P` | `Ctrl+V×3`（字符级 `P` 无定位；行级 `P` 也只在当前位置粘） | A1,P0-5 |
| 计数作用于 `J` | `3J` | 连接 3 行 = `End,Space,Delete,←` **×(N−1)=2 次**（Vim：`2J` 连 2 行、`3J` 连 3 行） | A1,D9 |
| 计数作用于 `u` | `3u` | 撤销 3 次（`Ctrl+Z` ×3，按剩余预算截断） | A1 |
| 计数作用于 `.` | `3.` | 重复 3 次（见 §9 `N.` 行） | A1 |
| 计数不作用于 `gg` | `3gg` | 丢弃 3，`gg` 执行 | #2b |
| 计数不作用于 `ZZ` | `3ZZ` | 丢弃 3，`ZZ` 执行 | A1 |
| 计数作用于 `X` | `3X` | **一次**选中 3 个字符再复制+删（`Shift+←×3, Ctrl+C, Backspace`） | A1 |
| 计数作用于 `C/D` | `3C`/`3D` | 作用到**下面第 N-1 行的行尾**：`Shift+End, Shift+Down×2, Shift+End` + `Ctrl+X`（`3C` 再进 Insert） | A1 |
| 计数作用于 `Y` | `3Y` | **≡ `3yy`（行级）**：`Home,Home,Shift+Down×3,Ctrl+C,Esc,Up×3` | A1,D8 |
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
| 进入并删 | `v` `l` `d` | `Shift+→`（`v` 预选光标下 1 字符）→ `Shift+→`（`l`）→ `Ctrl+X`（`d`，直接作用当前选区）→ 回 Normal | §4.9 动作后退出 |
| 选择复制 | `v` `e` `y` | 扩展选区 → `Ctrl+C` → 回 Normal | §4.9 |
| 行选进入 | `V` | `Home`,`Shift+End` → 选中整行（`off=0`，DOWN 态） | §4.9 |
| 行选向下 | `V` `j` | `Shift+Down`,`Shift+End`（DOWN 态直接扩展） | §4.9 |
| 行选向下计数 | `V` `3` `j` | 一次移动 3 行：`Shift+Down`×3,`Shift+End`（不是 3× 基础序列） | §4.8 §4.9 |
| 行选向上（方向翻转 → 重锚） | `V` `k` | 进入 `Home`,`Shift+End` 后 `Down`×1,`Home`,`Shift+Up`×2（锚移到 A+1 行首，选区含 A 行换行；**直接从当前光标重建**，不做冗余 `Shift+Up` 再重锚） | §4.9 |
| 行选向上后回下 | `V` `k` `j` | 接上行后 `Shift+Down`（`off=0` 仍处 UP 态，不重锚） | §4.9 |
| 行选向下越过锚点回锚 | `V` `k` `j` `j` | `Shift+Down`（第一个 `j`，`off=0`）；第二个 `j` 使 `off=+1>0` → 重锚回 DOWN：`Home`,`Shift+Down`,`Shift+End` | §4.9 |
| 行选不改行范围 | `V` `h` / `V` `l` / `V` `0` / `V` `^` / `V` `$` | **0 输出**（真实 Vim 里行范围不变） | §4.9 |
| 行选动作-复制 | `V` `y` | `Shift+Right`,`Ctrl+C`,`Esc` → 回 Normal（linewise，含换行） | §4.9 |
| 行选动作-删除 | `V` `d` / `V` `x` | `Shift+Right`,`Ctrl+X` → 回 Normal（**删掉整行**，不是只清正文） | §4.9 |
| 行选动作-修改 | `V` `c` / `V` `s` | `Shift+Right`,`Ctrl+X`,`Shift+Enter` → Insert（**c 与 s 完全等价**） | §4.9 |
| 行选动作-粘贴 | `V` `p` | `Shift+Right`,`Ctrl+V`,`Esc` → 回 Normal | §4.9 |
| 行选多行复制 | `V` `j` `y` | `Shift+Down`,`Shift+End`,`Shift+Right`,`Ctrl+C`,`Esc` | §4.9 |
| 行选向上多行复制 | `V` `k` `y` | UP 态：不补 `Shift+Right`（选区已含换行），`Ctrl+C`,`Esc` | §4.9 |
| 行选文末 | `V` `G` `y` | `Ctrl+Shift+End`,`Shift+Right`,`Ctrl+C`,`Esc` | §4.9 |
| 行选文首 | `V` `gg` `y` | `Down`,`Home`,`Ctrl+Shift+Home`,`Ctrl+C`,`Esc`（UP 态不补 `Shift+Right`） | §4.9 |
| 行选 Esc | `V` `Esc` | 发 `Esc` 取消宿主残留选区 → 回 Normal | §4.9 |
| 行选模式切换 | `v` `V` / `V` `v` | `v V` = `Home`,`Shift+End` 切行选；`V v` = 0 输出切回字符选 | §4.9 |
| 行选非法键 | `V` `i` | 0 输出、留在 VISUAL_LINE、无 pending | §4.9 |
| 可视计数 | `v` `3` `j` / `v` `2` `w` | 3×`Shift+↓` / 2×`Ctrl+Shift+→` | §4.8 §4.9 |
| 计数上限 | `v` `1` `2` `3` `j` | 第 3 位忽略 → 12×`Shift+↓` | §5 ≤2 位 |
| 计数含 0 | `v` `1` `0` `j` | `0` 续接计数 → 10×`Shift+↓` | §4.3 §5 |
| 非法键吞计数 | `v` `3` `i` `j` | `i` 吞掉计数 → 1×`Shift+↓` | §4.9 |
| `gg`（两可视模式） | `v` `g` `g` / `V` `g` `g` | `Ctrl+Shift+Home` | §4.9 |
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
| **repeat 不跨模式污染** | `2d` → `kv_set_mode(INSERT)` → `kv_set_mode(NORMAL)` → `w` → `.` | `.` 既不是 `2dw` 也不是 `w`（移动不记录）；无更早修改则 0 输出 | §4.7 |
| **`.` 目标是上一次修改** | `dw` → `w` → `.` | `.` 回放 `dw`（裸移动不夺走目标） | §4.7 |
| **`.` 忽略纯复制** | `x` → `yy` → `.` | `.` 回放 `x`（`y` 不是修改） | §4.7 |
| **`.` 忽略 `Y`** | `x` → `Y` → `w` → `.` | `.` 回放 `x` | §4.7 |
| **缩进是修改** | `>>` → `w` → `.` | `.` 回放 `>>` | §4.7 |
| **粘贴是修改** | `p` → `w` → `.` | `.` 回放 `p` | §4.7 |
| **连接是修改** | `J` → `w` → `.` | `.` 回放 `J` | §4.7 |
| **只有移动时 `.` 无动作** | `w` `j` `3j` `gg` → `.` | 0 输出 | §4.7 |
| **`N.` 重复 N 次** | `dw` → `3.` | 回放 `dw` 三次（6 键） | §4.7 |
| **`N.` 按队列封顶** | `dd` → `99.` | 回放 `dd` ⌊99/5⌋=19 次（95 键），不溢出丢键 | §4.7 |
| **无 `s_last` 时 `N.`** | `.` / `3.`（未做过修改） | 0 输出 | §4.7 |
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
| 裸 Caps 单击 | `Caps` 单击（无 Fn） | **无任何效果** | caps/readme §2 |
| Fn+Caps 单击 | `Fn` 按住 + `Caps` 单击 | 切换 vim 开/关（开=Insert 起） | caps/readme §2 |
| Caps 按下即入模式 | `Caps` 按下（不等阈值） | 进入 Caps 模式；`1`→`F1` | caps/readme §2 |
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
| E1 | `dd` 末行 / 首行 | 末行可删；**首行不留空行**（`Shift+→` 纳入换行，实测与 Vim 一致） |
| E2 | 修饰键释放被吞后继续命令 | 不再反复重装修饰位、不卡 `Shift` |
| E3 | `Alt+Tab` | Tab 不卡（key-up 对称透传） |
| E4 | `dd` 后 `u` | `dd` 现为**单次**宿主编辑 ⇒ 一次 `u` **完整恢复**（旧"方案 A 半恢复"随 `Backspace` 版 `dd` 一并作废） |
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

## 14. 矩阵对拍：真 Vim（`make matrix-test`）

> 仓库内自包含的端到端对拍：把引擎按当前 `engine/src/*.c` 发出的**宿主键码流**喂给
> `engine/test/host/kvhost.py` 的宿主编辑器模型，再与真实 `/usr/bin/vim.tiny`（VIM 9.1）
> 对同一按键序列的结果逐例比较（**共 598 例**：Normal / Visual / Visual-Line / 粘贴寄存器）。
> 工程流程、验收基线与棘轮语义的权威表述见 [`../qmk/engineering-spec.md`](../qmk/engineering-spec.md) §2/§3。

运行（在 `engine/` 下）：

- `make matrix-test` —— 先编译 `test/host/probe`，再跑完整矩阵；
- `python3 test/host/matrix.py --all` —— 打印每例（含通过）的明细。

比较三个维度，任一不符即该例不通过：

| 维度 | 模型侧 | Vim 侧 |
|---|---|---|
| 缓冲区 | 宿主模型编辑后的全文 | `:w!` 写出的全文 |
| 无名寄存器 | 宿主剪贴板文本 | `p` 还原出的寄存器文本 |
| 光标 | 绝对偏移换算的（行,列） | 插入 `@` 标记后的偏移 |

Vim 侧统一使用 `vim.tiny -Nu NONE -N -es -c 'set nofixendofline' -c "silent! normal! <KEYS>" -c 'w! OUT' -c 'qall!' IN`；
转义字节是**真实** `\x1b`（不是 `\e` 两个字符），且总是先 `gg` 到第 1 行（`-es` 下光标从末行开始）。

**xfail 归属**：`engine/test/host/matrix.py` 顶部的 `XFAIL` 表把用例名映射到**偏差编号**，
`DEVIATIONS` 表给出每个编号的含义 + 出处章节（`design.md` §4.4/§4.9、`readme.md`）。
现有编号既有 §4.9 VISUAL_LINE 的 ①–⑩，也有独立编号 `D2`/`D14`–`D18`/`D23`/`E-W`/`YCOL`/
`VCUR`/`PASTEC`/`EOLDEL`/`GPFX`/`FAILMOT`/`VCAP`/`VBLOCK`/`VPASTE`/`IND`。每个 xfail 都必须
写明编号以保证可审计。只有命中该表的用例才允许不符；其余不符一律判 FAIL，打印用例名/输入缓冲/
按键序列/模型结果/Vim 结果。表中已能对上真 Vim 的用例以 `XPASS` 列出，提示删除。

**退出码**：0 = 全部通过或全部命中已声明偏差；非 0 = 存在未声明的不符、或存在 XPASS/可删除的
棘轮条目。`make test` / `make glue-test` 不受影响。

## 15. 矩阵测试的 ratchet 基线（`engine/test/host/known_failures.txt`）

`make matrix-test` 用**棘轮（ratchet）**语义，既保持可用（今天为绿），又不会掩盖回归：

| 情况 | 结果 |
| :--- | :--- |
| 某用例**不在** `known_failures.txt` 里且与 Vim 不一致 | **硬失败**（新回归） |
| 某用例在表里，但**失败维度多出**未记录的维度 | **硬失败**（已知坏用例里出现新的损坏） |
| 表里的用例现在**完全一致** | **硬失败**，要求删除该行（表只能变小） |
| `matrix.py` 的 `XFAIL`（已声明偏差）里的用例现在一致 | **硬失败**，要求删除（同严格 xfail） |

格式：`<用例名> <失败维度>`，维度 ∈ `buf`（缓冲区）、`reg`（无名寄存器）、`cur`（光标），如
`yyp cur`、`dw buf+reg`。

**重名陷阱（判定规则）**：少数用例名重复（同一名字、两个缓冲区），因此**不能按名字建索引**判定
通过/失败。正确判据是"该名字是否**还有任何**不符实例"：
- XPASS 只在某名字**没有任何**失败实例时才算（否则"一个通过、一个不符"会被误判成 XPASS，
  进而错误地要求删表）；
- 基线条目同理，只在**没有任何**同名实例仍失败时才算"已修好，应删行"。
新增用例时**名字必须唯一**（生成器已用 `_` 转义空格）。

**为什么有这张表**：598 个用例里仍有一批与 Vim 不一致，其中大部分是 `design.md` §4.4/§4.9
或 `readme.md` 里**已经声明**的偏差，只是还没逐条搬进 `XFAIL`；其余是纯键码层**固有**的
（引擎读不到缓冲区边界/长度）或仍待修的缺陷。把它们留在表里可让门禁立刻可用；
**优先把条目搬进 `XFAIL` 并引用声明**，只有确实无法引用时才留在表里。


【当前状态（2026-10-01，HEAD `7be99d1`）】基线**已排空**（只剩头部说明）：实测
`TOTAL 598 PASS 405 XFAIL 193 KNOWN-FAIL 0 NEW-FAIL 0`、**退出码 0**（无 XPASS）。193 个不符用例
全部已逐条引用 `DEVIATIONS` 里的书面声明，因此棘轮基线里已没有"未引用"项；门禁现在等价于
**严格 xfail**：任何未声明的不符都会硬失败（不再被基线掩盖）。
> 注：`matrix.py` 只在**非空**时打印 `XPASS ...`/`NEW FAILURES ...`/`FIXED ...` 行，所以"绿"的
> 输出里**没有** `XPASS 0` 这个字样；判绿看退出码 0 与 `KNOWN-FAIL 0 NEW-FAIL 0`。

【名字冲突】生成器曾把 `'yl l p'` 与 `'yllp'` 都命名为 `pp-yllp`，使后者被隐藏（一个通过、一个不符，而 XFAIL 按名字建索引）。现用 `_` 保证名字唯一，并删掉 `'yl l p'` —— 它含**字面空格键**，而宿主模型未实现空格作为移动，该用例测的是模型而不是引擎。
已验证门禁**有效**（不是橡皮图章）：注入一个语义变异（去掉行级 `p` 的 `End,→` 定位）后
`make matrix-test` 由 0 变 2、报出 `NEW-FAIL 8`。

【`matrix.py` 里的陈旧说明文字（代码，本次未改）】`DEVIATIONS` 中 `D17` 的说明仍写"cgg@行0
切掉整行"，而 `design.md` §4.9 ⑭ 已实测它不再构成偏差；`VBLOCK` 的说明仍写"OPEN (real defect,
not yet fixed)"，而 `design.md` §4.9 的 D24 已修好（`vis-ky`/`v-k-y` 已通过、无用例引用它）。
两者都属 `engine/test/host/matrix.py`（代码），本次审计**只登记不改动**；下次改该文件时应同步
说明文字或删条目。`③`/`⑥`/`⑩` 三个编号当前无用例引用（合法保留：无实例触发）。
