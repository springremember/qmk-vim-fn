# 共享层 ↔ 键盘分支：同步与验证规范

> 本文件是**流程规范**（不是脚本）。权威顺序：本规范 + [`../vim/design.md`](../vim/design.md) §0/§4.12；
> 具体键位/灯位属于各键盘 keymap 的 readme。
> 位置：`qmk/README.md`。适用于 `qmk-vim-fn`（共享层仓库）与 `qmk_firmware`（每个键盘一个分支）。

---

## 1. 角色与约束

| 角色 | 仓库 / 位置 | 拥有什么 |
| :--- | :--- | :--- |
| **共享层（唯一源）** | `qmk-vim-fn`（`engine/` + `qmk/` + `vim/` + `fn/`，远端 `origin`） | 引擎、QMK 适配层、规格文档；**任何跨键盘行为只在这里实现** |
| **键盘分支** | `qmk_firmware` 的 `qk61` / `nut65`（远端 `newfork`） | 只拥有**键盘专属**部分：keymap、`config.h`、键盘 readme、厂商代码；共享层通过 **git 子模块**引用 |
| **交接方式** | 子模块路径 | `keyboards/qk61/keymaps/vim/qmk-vim-fn`、`keyboards/leku/nut65/keymaps/vim/qmk-vim-fn` |

硬性约束：

1. 共享层**禁止**出现键盘专属代码/测试；键盘分支**禁止**复制共享层实现。
2. 行为/接口变更必须**文档先行**（见 §2）。
3. **两个键盘分支引用的子模块必须是同一个提交**（同一份共享层，禁止各自漂移）。
4. 键盘分支只改本键盘的树；**禁止**把另一个键盘的目录带进来。

---

## 2. 文档先行（硬性顺序）

任何行为/接口变更，**必须**按此顺序，缺一不可：

1. **先改文档**：`vim/design.md`（架构/接口，唯一权威）、`vim/readme.md`（目标行为/状态指示），
   必要时 `vim/changes.md`（变更与缺陷记录）、`vim/testcase.md`（用例清单）、`fn/readme.md`。
2. **文档单独提交**：该提交**只含文档**，且**早于**任何源码提交。
   同一提交里既改文档又改源码 = **回填，违规**。
3. **测试次之**：用例按已提交的文档写，可单独提交。此阶段测试**应当编译失败**（红），
   这本身就是"测试先行"的证据。
4. **才允许改代码**：`engine/` + `qmk/` 实现对齐已提交的文档；**实现提交里不得夹带文档改动**
   （含头文件里的契约注释——注释也是代码）。
5. **同步到键盘分支**（§3）→ 键盘侧实现 → 编译归档（§4）→ 验证（§5）。

> **为什么**：历史上违反顺序导致共享层与键盘子模块各自漂移出一份**内容相同、哈希不同**的子模块提交，
> 两块键盘事实上跑的是两套共享层；也出现过"文档承诺 `cfg` 字段、代码却只写不读"的契约空转。

---

## 3. 同步（文档/实现提交 → 键盘分支）

前置：共享层已提交 **且已 `push`**（未 push 的提交别的机器/CI 取不到）。

对**每个**键盘分支重复：

```sh
cd <qmk_firmware>
git checkout <分支>                       # qk61 / nut65
git submodule update --init <子模块路径>   # 工作区可能被上次切分支清掉
cd <子模块路径>
git fetch origin
git checkout --quiet <共享层提交>          # 必须是两键盘共用的同一个提交
cd <qmk_firmware>
git add <子模块路径>                       # ★ gitlink 不会自己变，必须显式 stage
git commit -m "<键盘>: bump qmk-vim-fn -> <提交>（文档先行：规格/测试/实现）"
```

要点：

- `git submodule update --init` **只把工作区恢复到"分支上记录的旧 gitlink"**；要换新提交必须
  再 `fetch` + `checkout`，然后 **`git add` 子模块路径**把新 gitlink 写进分支。
- 切换键盘分支会清掉另一个键盘的子模块工作区；切回后必须重新 `submodule update --init`。

---

## 4. 编译与产物归档

```sh
export PATH=<项目根>/toolchain/usr/bin:$PATH
export QMK_HOME=<qmk_firmware 目录>
cd "$QMK_HOME" && rm -rf .build
make qk61:vim:bin                          # 或 make leku/nut65:vim:bin ALLOW_WARNINGS=yes
```

- **QK61/FS026 镜像体积敏感**：>~0x13F58(81752B) 会导致有线 USB 枚举失败；本 keymap 用
  `LTO_ENABLE=yes` 压在 ~73–79KB。改动后必须看 `Size after:` 并留在安全区。
- ⚠️ **`:bin` 目标会静默把 `$(TARGET).bin` 复制到仓库根**（chibios `platforms/chibios/platform.mk`
  的 `bin:` 规则，**没有** "Copying…" 提示），`.gitignore` 忽略 `*.bin`，所以 `git status`
  仍显示干净、极易漏检。`rm -f` 目录根的 `*.bin` **不是可选项**，每次编译后都要做并确认。
- `.hex` 只留在 `.build/`（`:bin` 不产生根 `.hex`）。
- 归档（**bin/hex 按版本另存**，写入 `.gitignore` 白名单外的 `output/` 需 `git add -f`）：

  | 键盘 | 产物 |
  | :--- | :--- |
  | QK61 | `output/qk61_vim_vX.Y.{bin,hex}` + `_via.json`（= `keyboards/qk61/CIDOO QK61 VIA.JSON`） |
  | NUT65 | `output/leku_nut65_vim_vX.Y.{bin,hex}` + `_via.json`（= `keyboards/leku/nut65/NUT65.json`） |

- 发布提交与实现提交**分开**；打 **注解 tag**（`vX.Y-<键盘>`）并推送。

---

## 5. 验证清单（每次同步/发布都要跑）

```sh
# ① 共享层：本仓库干净 + 提交已 push
git -C <qmk-vim-fn> status --porcelain
git -C <qmk-vim-fn> ls-remote origin refs/heads/main      # == 本地 main

# ② 两分支引用的共享层提交必须相同
git -C <qmk_firmware> ls-tree qk61 <qk61 子模块路径>
git -C <qmk_firmware> ls-tree nut65 <nut65 子模块路径>

# ③ 子模块内容 == 共享层仓库工作区（内容一致性的硬证据）
#    子模块的 git 目录在 <qmk_firmware>/.git/modules/<子模块路径>；
#    直接 git --git-dir 会因 core.worktree 指向不存在路径而失败，用 --work-tree 绕过：
GD=<qmk_firmware>/.git/modules/<子模块路径>
for f in qmk/vim_keymap_common.c qmk/vim_keymap_common.h vim/design.md vim/readme.md; do
  a=$(sha256sum <qmk-vim-fn>/$f | cut -d' ' -f1)
  b=$(git --git-dir=$GD --work-tree=/tmp show <提交>:$f | sha256sum | cut -d' ' -f1)
  [ "$a" = "$b" ] || echo "不一致: $f"
done
#    更强：git --git-dir=$GD --work-tree=/tmp rev-parse <提交>^{tree}  ==  共享层 HEAD^{tree}

# ④ 交叉污染（必须为 0）
git -C <qmk_firmware> ls-tree -r --name-only qk61  | grep -c '^keyboards/leku/'
git -C <qmk_firmware> ls-tree -r --name-only nut65 | grep -c '^keyboards/qk61/'

# ⑤ 主机测试（全绿才算完成）
make -C <qmk-vim-fn>/engine test          # 引擎单测
make -C <qmk-vim-fn>/engine glue-test     # 适配层/共享 keymap 层
cd <keymap>/test && make                  # 键盘侧

# ⑥ 固件重编 == 归档（逐字节）
git -C <qmk_firmware> show <分支>:output/<归档>.bin | cmp - .build/<TARGET>.bin
ls <qmk_firmware>/*.bin <qmk_firmware>/*.hex 2>/dev/null     # 必须为空
```

无法用主机测试覆盖的部分（键盘侧 **RGB 实际渲染**：颜色常量、灯位索引、亮度百分比、
低电/测试灯让位）**只能实机确认**；主机测试不得声称覆盖了它们。

---

## 6. 坑位清单（全部为实际发生过的事故）

| # | 坑 | 后果 | 规避 |
| :-- | :--- | :--- | :--- |
| 1 | 只改子模块 checkout、不在共享层仓库提交 | 两键盘各漂移出一个**哈希不同、内容相同**的子模块提交 | 先提交+push 共享层，再同步；§5②③ 校验 |
| 2 | `submodule update` 后忘了 `git add` 子模块 | 实现提交里的 gitlink 还是旧提交（静默） | §3 末尾显式 `git add`；§5② 校验 |
| 3 | 用 `git commit --amend` 提交子模块指针 | 把**发布提交**改成了 bump 提交（分支偏离远端） | 子模块 bump 用新提交；`amend` 前先看 `git log -1` 与 `git status` |
| 4 | 工作树/index 处于半途状态（手工 `read-tree`/`update-index` 后）就切换分支 | 陈旧 index 被当成上千条真实改动，**把另一个键盘的树灌进来** | 切分支前 `git status` 必须干净；必要时 `git reset --hard` 后再切 |
| 5 | 在 `qmk_firmware` 根跑 `:bin` 后不清理 | 根目录残留 `*.bin`，因 `.gitignore` 而 `git status` 仍干净 | 每次编译后 `rm -f` 并核对（§4） |
| 6 | 用 16 位 `timer_read()` 做长窗口计时 | **65536ms 回绕**让"已过期"窗口复活（实测：Esc 宽限窗口每 65.5s 假命中 3s，且期间 Esc 变真实宿主 Esc） | 凡窗口可能被"长时间不检查"跨越，一律 `timer_read32()/timer_elapsed32()`（design §4.12） |
| 7 | 文档承诺 `cfg` 契约、代码只写不读 | 改配置无效，行为与文档不符 | 契约必须在共享层落地并被测试覆盖（如 `vim_insert_flash_color()`） |
| 8 | 只比"提交号不同"就断言内容漂移 | 误判（两个提交内容可以完全相同） | 用**文件 sha256** 或 `HEAD^{tree}` 做内容判定 |
| 9 | 只跑测试不看测试覆盖 | "全绿"掩盖未覆盖风险（键盘 RGB 渲染） | 明确列出覆盖缺口，标注只能实机验证的部分 |

---

## 7. 独立审计（建议，非强制）

对涉及 P0/P1 或跨键盘的变更，建议由**未参与实现**的一方按上述 §5 独立复核，
并额外要求：自写独立测试（不复用实现者的断言）、给出反例、并明确列出覆盖缺口。
审计结论与整改同样走 §2 顺序（文档 → 测试 → 实现）。
