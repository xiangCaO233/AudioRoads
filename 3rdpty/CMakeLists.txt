if(SOURCES_BUILD)
  # 源码模式只进入已初始化的上游子模块，不读取 prebuilts，也不修改上游 CMake。
  # sources 必须导出与预编译模式相同的 3rd_* 包装 target，业务层无须分支。
  add_subdirectory("sources")
else()
  # 预编译模式只启用项目自维护 Find 脚本；缺少 ABI 产物必须配置失败，不能
  # 静默命中系统库或回退源码构建，否则同一开关不再可复现。
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
  # 新增依赖时源码和 Find 路径必须同时提供同名包装 target。
  find_package(glfw REQUIRED)
  find_package(imgui REQUIRED)
endif()
