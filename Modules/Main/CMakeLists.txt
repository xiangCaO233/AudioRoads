if(APPLE)
  # 最终 target 名在三平台保持一致；macOS 仅改变为 bundle 并从 project
  # 元数据填充标识和版本，供独立交付脚本安装完整 .app。
  add_executable(AudioRoads MACOSX_BUNDLE src/Application.cpp src/main.cpp)
  set_target_properties(
    AudioRoads
    PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "AudioRoads"
               MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.xiangcao233.audioroads"
               MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
               MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
elseif(WIN32)
  # WIN32 关键字选择 GUI 子系统，避免启动时出现额外控制台。 mainCRTStartup 只在 MSVC 分支设置，其他 Windows
  # 工具链使用自身默认。
  add_executable(AudioRoads WIN32 src/Application.cpp src/main.cpp)
  if(MSVC)
    set_target_properties(AudioRoads PROPERTIES LINK_FLAGS
                                                "/ENTRY:mainCRTStartup")
  endif()
else()
  # 现代 Linux 生成普通桌面可执行文件，桌面入口由交付脚本单独安装。
  add_executable(AudioRoads src/Application.cpp src/main.cpp)
endif()

# Main 是 Audio 与 UI 两条依赖链的唯一交汇点，只负责 Application 生命周期。
# 新进程级资源应由 Application 显式持有，不能在 CMake 中隐式创建全局对象。
target_include_directories(AudioRoads PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Main 头路径不传播给下层模块；target 依赖负责链接顺序，不拼接平台库路径。
# Threads::Threads 提供未来设备服务和流线程所需的平台链接选项，告警覆盖入口。
# 只有 AudioRoads 会被安装，所链接的内部静态库不导出为 SDK target。
target_link_libraries(AudioRoads PRIVATE AudioRoadsAudio AudioRoadsUI
                                         Threads::Threads AudioRoadsWarnings)
