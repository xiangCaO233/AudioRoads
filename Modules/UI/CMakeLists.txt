# 每个平台只编译一个系统字体解析器，避免 resolveSystemFontFaces 重复定义。
set(AUDIOROADS_SYSTEM_FONT_SOURCE "")
if(WIN32)
  # DirectWrite 定位字体文件，user32 提供当前用户的消息字体偏好。
  set(AUDIOROADS_SYSTEM_FONT_SOURCE src/SystemFont-windows.cpp)
elseif(APPLE)
  # CoreText/CoreFoundation 解析系统字体和字符级 cascade。
  set(AUDIOROADS_SYSTEM_FONT_SOURCE src/SystemFont-macos.cpp)
else()
  # Linux 通过 pkg-config 使用发行版提供的 Fontconfig。
  find_package(PkgConfig REQUIRED)
  # Fontconfig 是界面启动所需的必选系统依赖。
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
  set(AUDIOROADS_SYSTEM_FONT_SOURCE src/SystemFont-linux.cpp)
endif()

add_library(AudioRoadsUI STATIC src/AppWindow.cpp src/MainView.cpp
                                ${AUDIOROADS_SYSTEM_FONT_SOURCE})
# AppWindow 管理渲染生命周期，MainView 只编辑 Core 内存模型；此 target
# 不得链接 Audio 或任何平台音频后端。字体源显式加入，不预处理 include cpp。
target_include_directories(AudioRoadsUI
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# PUBLIC 仅向仓库内 Main 传播 Core 和内部头路径，不参与 install。
# GLFW、OpenGL、ImGui 与平台字体库都是实现细节，保持 PRIVATE。
target_link_libraries(
  AudioRoadsUI
  PUBLIC AudioRoadsCore
  PRIVATE 3rd_imgui OpenGL::GL AudioRoadsWarnings)

if(WIN32)
  # dwrite 负责字体文件解析，user32 负责系统界面字体偏好。
  target_link_libraries(AudioRoadsUI PRIVATE dwrite user32)
elseif(APPLE)
  # framework 使用 find_library 获得当前 SDK 的稳定链接引用。
  find_library(AUDIOROADS_CORE_TEXT CoreText REQUIRED)
  find_library(AUDIOROADS_CORE_FOUNDATION CoreFoundation REQUIRED)
  target_link_libraries(AudioRoadsUI PRIVATE "${AUDIOROADS_CORE_TEXT}"
                                             "${AUDIOROADS_CORE_FOUNDATION}")
else()
  # imported target 同时传递 Fontconfig 的头路径和链接参数。
  target_link_libraries(AudioRoadsUI PRIVATE PkgConfig::FONTCONFIG)
endif()

if(BUILD_TESTING)
  # 测试直接复用产品的平台源和系统库，防止验证一个替代实现。
  # 它只包含 SystemFont.h 且不链接 ImGui，确保平台解析边界没有泄漏 UI 类型。
  add_executable(SystemFontTest tests/SystemFontTest.cpp
                                ${AUDIOROADS_SYSTEM_FONT_SOURCE})
  # 测试的内部头路径不对其他 target 传播。
  target_include_directories(SystemFontTest
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(SystemFontTest PRIVATE AudioRoadsWarnings)
  # 测试链接分支必须与 AudioRoadsUI 同源，否则某个平台只会在产品链接时失败。
  if(WIN32)
    # Windows 测试同样读取用户偏好并解析本地 DirectWrite 文件。
    target_link_libraries(SystemFontTest PRIVATE dwrite user32)
  elseif(APPLE)
    # Apple 测试沿用产品已找到的 framework 绝对引用，不重复查找另一套 SDK。
    target_link_libraries(
      SystemFontTest PRIVATE "${AUDIOROADS_CORE_TEXT}"
                             "${AUDIOROADS_CORE_FOUNDATION}")
  else()
    # Linux 测试复用同一 pkg-config imported target，包含路径和链接参数一致。
    target_link_libraries(SystemFontTest PRIVATE PkgConfig::FONTCONFIG)
  endif()
  # 注册名称与 target 一致，便于 ctest -R 精确筛选字体解析边界。
  add_test(NAME SystemFontTest COMMAND SystemFontTest)
endif()
