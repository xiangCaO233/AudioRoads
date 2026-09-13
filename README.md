# AudioRoads

AudioRoads 是一款面向 Windows、现代 Linux（PipeWire）和 macOS 的桌面音频
路由客户端。它把录音设备和单个桌面应用的播放流统一为 source，把物理播放
设备和系统可见虚拟麦克风统一为 target。用户在节点画布中点击两个方块即可
连线；多条连接汇入同一目标时完成混音，每条连接独立控制增益和静音。

当前仓库已经具备可运行的客户端外壳、三平台物理端点枚举、应用输出来源发现、
节点连线 UI、无分配混音内核及无锁 SPSC 音频缓冲。Linux PipeWire 后端已经可以
把物理输入或应用输出路由到指定物理播放设备，并实时响应每条连接的增益和静音。
Windows/macOS 实际平台流、长期跨设备时钟漂移校正及三平台虚拟麦克风组件仍在
实现中。

## 平台后端

| 平台 | 原生接口 | 当前能力 |
| --- | --- | --- |
| Windows | MMDevice / WASAPI | 活动设备及音频会话发现；后续用 application loopback 捕获进程树 |
| Linux | PipeWire 0.3 | 端点/应用流发现；来源到指定物理播放设备的实时路由与混音 |
| macOS | CoreAudio HAL | 设备及运行中输出进程发现；要求 macOS 14.2+ |

三个后端都只向上层暴露 `AudioSource`、`AudioTarget` 值类型；原生句柄不会进入
Core 或 UI。系统虚拟麦克风的组件边界和签名策略见
[平台端点组件](docs/platform-components.md)。

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

`SOURCES_BUILD` 是唯一来源开关，默认值为 `ON`。关闭时缺少预编译头文件或库会
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
