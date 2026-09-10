cmake_minimum_required(VERSION 3.31)

# 版本常量在 project() 之前定义，供 target 元数据与独立打包脚本共同使用， 避免生成产物和安装包各自维护版本。
set(AUDIOROADS_VERSION "0.1.0")

# 顶层只建立全局策略和模块装配；具体算法、UI 与平台后端 target 在子目录定义。 Objective-C/Objective-C++ 仅为 Apple
# framework 互操作启用，其他平台不能被 无谓要求安装对应编译器前端。
if(APPLE)
  project(
    AudioRoads
    VERSION ${AUDIOROADS_VERSION}
    LANGUAGES C CXX OBJC OBJCXX)
else()
  project(
    AudioRoads
    VERSION ${AUDIOROADS_VERSION}
    LANGUAGES C CXX)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES)
  # 只为单配置生成器维护 CMAKE_BUILD_TYPE；Visual Studio、Xcode 等多配置 生成器由构建阶段选择配置，不能在这里强制覆盖。
  # 单配置生成器保留用户传入的空或具体构建类型。 可选值列表只用于 GUI 和编辑器提示，不强制默认类型。
  set(CMAKE_BUILD_TYPE
      "${CMAKE_BUILD_TYPE}"
      CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release
                                               RelWithDebInfo MinSizeRel)
endif()

# C++23 是稳定基线；支持完整 C++26 模式的工具链可通过缓存变量显式切换。
set(AUDIOROADS_CXX_STANDARD
    "23"
    CACHE STRING "C++ language standard: 23 or 26.")
set_property(CACHE AUDIOROADS_CXX_STANDARD PROPERTY STRINGS 23 26)
if(NOT AUDIOROADS_CXX_STANDARD MATCHES "^(23|26)$")
  message(FATAL_ERROR "AUDIOROADS_CXX_STANDARD 只能是 23 或 26。")
endif()
set(CMAKE_CXX_STANDARD ${AUDIOROADS_CXX_STANDARD})
# 用全局属性约束所有自维护和可配置第三方 target，避免某个子目录遗漏标准设置。 拒绝编译器静默降级到旧语言标准。
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 禁用 GNU 扩展模式，保持三平台语言语义一致。
set(CMAKE_CXX_EXTENSIONS OFF)
# 当前代码未使用 C++ modules，避免上游源文件发生扫描。
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# 第三方依赖链接偏好与参照项目一致，仅接受 static 或 shared。
set(PROJECT_LINKAGE
    "static"
    CACHE STRING "Third-party dependency linkage preference.")
set_property(CACHE PROJECT_LINKAGE PROPERTY STRINGS static shared)
# 对用户输入做大小写归一化后再验证。
string(TOLOWER "${PROJECT_LINKAGE}" PROJECT_LINKAGE)
if(NOT PROJECT_LINKAGE MATCHES "^(static|shared)$")
  message(FATAL_ERROR "PROJECT_LINKAGE 只能是 static 或 shared。")
endif()

if(MSVC)
  # MSVC 运行库必须与第三方链接形态保持一致。 Debug 后缀通过生成器表达式选择，兼容多配置构建。
  if(PROJECT_LINKAGE STREQUAL "shared")
    # 动态依赖必须配套 DLL CRT；Debug 变体由配置表达式在生成期选择。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set(BUILD_SHARED_LIBS
        ON
        CACHE BOOL "Build shared third-party libraries." FORCE)
  else()
    # 静态依赖配套静态 CRT，防止跨模块分配对象时混用运行库堆。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(BUILD_SHARED_LIBS
        OFF
        CACHE BOOL "Build static third-party libraries." FORCE)
  endif()
elseif(PROJECT_LINKAGE STREQUAL "shared")
  # 非 MSVC 的 shared 路径通过 BUILD_SHARED_LIBS 传给可配置上游。
  set(BUILD_SHARED_LIBS
      ON
      CACHE BOOL "Build shared third-party libraries." FORCE)
else()
  # 静态是默认依赖形态，与预编译根目录布局一致。
  set(BUILD_SHARED_LIBS
      OFF
      CACHE BOOL "Build static third-party libraries." FORCE)
endif()

# 可执行文件统一放在构建树 bin 目录。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# 动态库产物与可执行文件同目录，便于 Windows 运行时查找。
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# 静态归档库统一放在构建树 lib 目录。
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# 项目不用 Presets，所有用户可控入口在根文件显式声明并描述降级边界。 本仓库默认从源码构建第三方依赖，且 SOURCES_BUILD 是唯一来源开关。
option(SOURCES_BUILD "Build third-party dependencies from sources." ON)
# 测试默认关闭，开发和 CI 需显式开启。
option(BUILD_TESTING "Build and register AudioRoads tests." OFF)
# PipeWire 开关只在 Linux 分支消费，其他平台不受影响。
option(AUDIOROADS_ENABLE_PIPEWIRE
       "Enable the native PipeWire backend on Linux." ON)

if(BUILD_TESTING)
  # enable_testing 必须在各模块 add_test 之前执行。
  enable_testing()
endif()

# 在第三方 GLFW/ImGui target 之前准备 OpenGL imported target。 REQUIRED
# 让缺少桌面渲染栈的环境在配置期给出明确失败，而不是产生残缺客户端。
find_package(OpenGL REQUIRED)
# 线程 imported target 也会被 GLFW 的预编译包装层复用。
find_package(Threads REQUIRED)

# OpenGL 是当前 ImGui 渲染后端的必需依赖。 Threads 使用 CMake 平台 target，避免硬编码 pthread 链接参数。
# 只把严格告警应用到自维护目标，避免为原样上游源码制造无关噪声。 INTERFACE target 使每个内部模块显式选择策略，而不会污染
# 3rdpty/sources。
add_library(AudioRoadsWarnings INTERFACE)
if(MSVC)
  # MSVC 采用原生告警选项；/permissive- 收紧标准一致性但不影响上游源码。
  target_compile_options(AudioRoadsWarnings INTERFACE /W4 /permissive-)
else()
  # GCC/Clang 共用可移植告警集合，conversion 与 shadow 提前暴露音频数值风险。
  target_compile_options(AudioRoadsWarnings INTERFACE -Wall -Wextra -Wpedantic
                                                      -Wconversion -Wshadow)
endif()

# 依赖装配顺序不可颠倒：第三方包装 target 先建立，Modules 内再按 Core、Audio、UI、Main 的单向依赖加入。
add_subdirectory("3rdpty")
# 业务模块只链接稳定 3rd_* 包装 target。
add_subdirectory("Modules")

# 最终客户端 target 存在后才声明安装和打包规则；根文件不安装 SDK 头文件。 交付规则单独维护，避免编译目标定义被平台打包分支淹没。
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallAndPackage.cmake")
