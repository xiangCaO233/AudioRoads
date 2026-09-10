# AudioRoads 智能体指令

## 核心边界

- 修改前先用 `rg` 和 `rg --files` 理解调用关系，只处理用户授权范围。
- 所有命令从仓库根目录执行，文件操作使用绝对路径。
- `3rdpty/sources/*` 是无下游补丁的上游 Git 子模块，禁止修改或格式化。
- 本项目是完整客户端，不安装 SDK 头文件，也不建立仓库根 `include/`。

## 构建与依赖

- CMake 最低版本为 3.31，稳定语言基线为 C++23；C++26 通过
  `AUDIOROADS_CXX_STANDARD=26` 显式开启。
- 不使用 CMake Presets。开发构建使用直接的 `cmake -S/-B` 命令。
- `SOURCES_BUILD` 是第三方来源的唯一开关，默认 `OFF`：`ON` 从
  `3rdpty/sources` 构建，`OFF` 只允许 `3rdpty/prebuilts`，不得静默回退。
- 预编译布局、独立 `Find*.cmake` 和包装 target 必须与当前
  `3rdpty/cmake/modules` 约定保持一致，业务模块不得感知依赖来源。
- 修改后至少执行构建；测试相关修改还需运行
  `ctest --test-dir build --output-on-failure`。
- 本机构建并行度不超过 `max(1, floor(nproc * 60 / 100))`。

## C++ 规则

- 禁止 `throw`、`try`、`catch`；使用返回值、`std::expected` 或
  `std::optional` 表达失败。
- 禁止显式 `new`/`delete`；平台句柄使用 RAII 所有者，跨层状态优先值语义。
- 命名空间和类型使用 `PascalCase`，函数与变量使用 `camelCase`，成员使用
  `m_` 前缀，常量和宏使用 `UPPER_SNAKE_CASE`。
- 头文件使用 `#pragma once` 并保持自包含；对应头优先，其次项目、第三方、
  标准库头，禁止依赖传递包含。
- 类、函数和成员使用中文 Doxygen 注释；注释解释契约、线程、生命周期和原因。
- 实时音频回调中禁止分配、锁、日志、文件系统、阻塞等待和所有权复制；只使用
  预分配缓冲区、稳定 ID/索引及最弱可证明正确的同步。
- 修改的 C/C++ 文件必须运行仓库 `.clang-format`；自维护 CMake 使用
  `cmake-format`，CMake 注释使用中文。

## 架构方向

- `Core` 保存设备 DTO、路由图和无平台混音算法。
- `Audio` 通过 `IAudioBackend` 隔离 WASAPI、PipeWire 和 CoreAudio。
- `UI` 只能编辑内存模型，不得在逐帧函数直接调用平台音频 API。
- `Main` 只负责组装与生命周期；依赖方向不得反转。

## Git

- 保留无关工作树修改，不主动提交或推送。
- 提交格式为 `type(scope): 中文描述`，冒号前使用英文类型与 scope。
