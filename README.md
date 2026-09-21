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

## 构建 / 测试引擎

```sh
make -C engine test
```

## 许可

MIT，见 [`LICENSE`](LICENSE)。
