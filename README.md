# qmk-vim-fn

键盘无关的 **vim 引擎** + **功能键（myfn）约定**。

- `vim/` — vim 引擎（源自 qmk-vim）：使用说明、技术架构与实现、变更记录、测试用例。
- `fn/` — 功能键约定（qmk-myfn）：按住 `Fn` 时各键做什么。

## 目录

| 文件 | 内容 |
|---|---|
| [`vim/readme.md`](vim/readme.md) | vim 模式**使用说明**（面向使用，描述目标行为） |
| [`vim/design.md`](vim/design.md) | 引擎**技术架构与实现细节** |
| [`vim/changes.md`](vim/changes.md) | 变更与问题记录 |
| [`vim/testcase.md`](vim/testcase.md) | 测试用例（含历史问题回归） |
| [`fn/readme.md`](fn/readme.md) | myfn 约定**使用说明** |
| [`fn/changes.md`](fn/changes.md) | myfn 变更记录 |

## 说明

- 本仓库描述的是**目标行为**（重构后的引擎与约定），与旧实现的行为差异集中记录在
  [`vim/design.md`](vim/design.md) 与 [`vim/changes.md`](vim/changes.md)。
- 引擎与具体键盘解耦；键盘层的键位（Fn 层、鼠标模式触发键等）由各键盘 keymap 实现。

## 许可

见 [LICENSE](LICENSE)。
