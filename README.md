# qmk-vim-fn

统一键盘功能仓库：**vim 引擎** + **myfn 约定**。

- `vim/` —— vim 引擎的设计、使用、变更与测试。
  - [`vim/readme.md`](vim/readme.md)：使用说明（目标行为）。
  - [`vim/design.md`](vim/design.md)：技术架构与实现细节（**唯一权威**）。
  - [`vim/changes.md`](vim/changes.md)：变更与历史问题。
  - [`vim/testcase.md`](vim/testcase.md)：测试用例。
- `fn/` —— myfn 约定（与键盘无关的 Fn 键行为约定）。
  - [`fn/readme.md`](fn/readme.md)：约定内容。
  - [`fn/changes.md`](fn/changes.md)：版本记录。
- `engine/` —— vim 引擎核心（与 QMK 解耦的纯 C，附主机单测）。
- `caps/` —— **Caps 长按模块规格**（键盘层模式：长按 Caps 期间的键映射）。
  - [`caps/readme.md`](caps/readme.md)：目标行为。
  - [`caps/design.md`](caps/design.md)：设计与实现规格（**唯一权威**）。
  - [`caps/changes.md`](caps/changes.md)：版本与缺陷记录。
  - [`caps/testcase.md`](caps/testcase.md)：测试用例。
- `qmk/` —— QMK 适配层与**流程规范**。
  - [`qmk/README.md`](qmk/README.md)：**共享层 ↔ 键盘分支的同步与验证规范**
    （文档先行顺序、子模块同步、编译归档、验证清单、坑位清单）。
  - [`qmk/engineering-spec.md`](qmk/engineering-spec.md)：**工程规范与验收基线**
    （文档先行/测试先红/变异验证/独立复核、`make test`/`glue-test`/`matrix-test` 的实测数字、
    矩阵与棘轮语义、键码预算规范、体积评估结论、发布判据、真机未验证清单）。
  - [`qmk/on-device-checklist.md`](qmk/on-device-checklist.md)：**真机验收清单**（主机测试覆盖不到
    的部分：键码节流/高计数命令、右 Shift 懒发送、Caps 合成 Ctrl、宿主列保持与选区保持、
    末行无尾换行、3 秒 Esc 宽限、7 色可区分度、NUT65 深睡/无线/bootloader、QK61 枚举/RAM 余量、
    刷机前归档核对），逐条给出操作步骤、预期结果与结果栏。

## 构建 / 测试引擎

```sh
make -C engine test        # 引擎单测
make -C engine glue-test   # QMK 适配层 / 共享 keymap 层
make -C engine matrix-test # 与真实 vim.tiny 的矩阵对拍
make -C engine verify-all  # 一次跑完全部四道门禁（顺序固定、首个失败即停、末尾汇总）
```

> 当前实测基线与判据（含"什么算绿"）见 [`qmk/engineering-spec.md`](qmk/engineering-spec.md) §2。
> `make verify-all` 会在收尾自动还原 `make glue-test` 改写的两个已跟踪测试二进制；
> 手工单跑 `glue-test` 时才需要自己 `git checkout --` 还原。
> **主机测试全绿 ≠ 真机通过**：真机上才能确认的部分按
> [`qmk/on-device-checklist.md`](qmk/on-device-checklist.md) 逐条走。

## 改动的硬性顺序（文档先行）

1. 先改文档（`vim/design.md` / `vim/readme.md`、`caps/design.md` / `caps/readme.md`，
   必要时各自的 `changes.md`、`testcase.md`）；
2. **文档单独提交**（只含文档，早于源码）；
3. 再写测试（此阶段应编译失败 = 测试先行的证据）；
4. 才改 `engine/` + `qmk/` 实现（不得夹带文档）；
5. 同步到键盘分支、编译归档、按 [`qmk/README.md`](qmk/README.md) §5 验证。

> 完整流程与踩坑记录见 [`qmk/README.md`](qmk/README.md)。

## 许可

MIT，见 [`LICENSE`](LICENSE)。
