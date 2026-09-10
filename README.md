# AudioRoads

AudioRoads 是一款面向 Windows、现代 Linux（PipeWire）和 macOS 的桌面音频
路由客户端。它把物理声卡、虚拟设备与系统端点统一为设备快照，让用户在图形
界面中创建 `输入 -> 增益/静音 -> 输出` 路由，并为后续多路实时混音、重采样和
通道映射提供稳定边界。

当前仓库已经具备可运行的客户端外壳、三平台原生设备枚举、路由编辑 UI、无分配
混音内核和基础测试。实际设备流的打开、跨时钟域缓冲和实时路由执行仍是下一阶段，
因此当前版本不会宣称已经把创建的路由送入声卡。

## 平台后端

| 平台 | 原生接口 | 当前能力 |
| --- | --- | --- |
| Windows | MMDevice / WASAPI | 活动输入输出端点、默认端点、共享格式发现 |
| Linux | PipeWire 0.3 | registry 音频 Source/Sink 节点发现 |
| macOS | CoreAudio HAL | 设备 UID、输入输出通道、默认设备和采样率发现 |

三个后端都只向上层暴露 `AudioDevice` 值类型；原生句柄不会进入 Core 或 UI。

## 构建

项目不使用 CMake Presets。首次开发构建需要初始化源码依赖：

```bash
git submodule update --init --recursive
cmake -S . -B build -G Ninja \
  -DSOURCES_BUILD=ON \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
```

Linux 还需要 CMake、Ninja、Clang、OpenGL/Wayland 或 X11 开发文件以及
Fontconfig 开发文件（Debian 上为 `libfontconfig1-dev`）和
`libpipewire-0.3-dev`。如果构建机只需要检查 Core/UI，可以显式添加
`-DAUDIOROADS_ENABLE_PIPEWIRE=OFF`；该模式会在客户端内明确显示后端不可用，
不能用于产品构建。

按仓库约定，构建并行度最多使用逻辑 CPU 的 60%：

```bash
cpu_count=$(nproc)
build_jobs=$((cpu_count * 60 / 100))
if [ "$build_jobs" -lt 1 ]; then build_jobs=1; fi
cmake --build build --parallel "$build_jobs"
ctest --test-dir build --output-on-failure
```

稳定语言基线是 C++23。支持相应标准库的工具链可添加
`-DAUDIOROADS_CXX_STANDARD=26` 试用 C++26。

## 第三方依赖

依赖引入方式和目录布局与 `MusicMapMaker-Next` 一致：

```text
3rdpty/
├── cmake/modules/Find<package>.cmake
├── prebuilts/
│   ├── headers/<package>/include
│   └── binaries/<platform>/<package>/libs/<arch>/<toolchain>/<tag>/<config>
└── sources/
    ├── cmake/<package>.cmake
    └── <package>                   # Git submodule
```

`SOURCES_BUILD` 是唯一来源开关，默认值为 `OFF`。关闭时缺少预编译头文件或库会
直接配置失败，不会偷偷回退到源码；开启时从 `3rdpty/sources` 构建。两条路径都
导出同名 `3rd_*` target，业务模块不感知来源差异。

## 代码布局

```text
Modules/
├── Core/src       # 设备 DTO、路由图、实时安全混音算法
├── Audio/src      # IAudioBackend 与三平台原生实现
├── UI/src         # GLFW/OpenGL/ImGui 桌面工作区
└── Main/src       # 进程入口与模块组装
```

这是应用程序仓库，没有供其它软件安装的根 `include/`，也不安装开发头文件。
详细线程和后续实时流设计见 [架构说明](docs/architecture.md)。
