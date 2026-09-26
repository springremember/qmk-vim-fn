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
- `qmk/` —— QMK 适配层与**流程规范**。
  - [`qmk/README.md`](qmk/README.md)：**共享层 ↔ 键盘分支的同步与验证规范**
    （文档先行顺序、子模块同步、编译归档、验证清单、坑位清单）。

## 构建 / 测试引擎

```sh
make -C engine test        # 引擎单测
make -C engine glue-test   # QMK 适配层 / 共享 keymap 层
```

## 改动的硬性顺序（文档先行）

1. 先改文档（`vim/design.md` / `vim/readme.md`，必要时 `vim/changes.md`、`vim/testcase.md`）；
2. **文档单独提交**（只含文档，早于源码）；
3. 再写测试（此阶段应编译失败 = 测试先行的证据）；
4. 才改 `engine/` + `qmk/` 实现（不得夹带文档）；
5. 同步到键盘分支、编译归档、按 [`qmk/README.md`](qmk/README.md) §5 验证。

> 完整流程与踩坑记录见 [`qmk/README.md`](qmk/README.md)。

## 许可

MIT，见 [`LICENSE`](LICENSE)。
