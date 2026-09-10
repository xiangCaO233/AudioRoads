# 预编译 ImGui 必须按公共 ABI 布局解析，禁止为通过配置而回退系统安装版本， 否则 headers 与二进制可能来自不同版本。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 布局维度在解析 ImGui 头和库之前统一初始化。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 当前预编译库包含 GLFW/OpenGL3 backend，因此先解析稳定 3rd_glfw target。 新增 backend
# 时必须同时更新源码构建和预编译包，不能在此补丁上游头文件。
find_package(glfw REQUIRED)
# ImGui 核心和 backend 头共享同一包头根。
prebuilt_include_dir(_imgui_include_dir imgui)

if(NOT TARGET imgui-static)
  # UNKNOWN 兼容各平台库后缀，GLOBAL 允许 UI 从同级子目录引用。
  add_library(imgui-static UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    imgui-static
    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
               "${_imgui_include_dir};${_imgui_include_dir}/backends")
  prebuilt_target_configs(_imgui_configs)
  # 导入配置列表不包含未找到的伪配置。
  set(_imgui_imported_configs "")
  # 通用位置默认为空，首个解析库才可赋值。
  set(_imgui_default_library "")
  foreach(_config IN LISTS _imgui_configs)
    # 每个生成器配置显式绑定同 ABI 产物，Debug 不会被发布库替代。
    string(TOUPPER "${_config}" _config_upper)
    prebuilt_find_library(_library imgui "${_config}" imgui imgui-static)
    list(APPEND _imgui_imported_configs "${_config_upper}")
    set_target_properties(
      imgui-static PROPERTIES "IMPORTED_LOCATION_${_config_upper}"
                              "${_library}")
    if(_imgui_default_library STREQUAL "")
      # 首个成功库也覆盖单配置未声明和 NOCONFIG 查找。
      set(_imgui_default_library "${_library}")
    endif()
  endforeach()
  set_target_properties(
    imgui-static
    PROPERTIES IMPORTED_CONFIGURATIONS "${_imgui_imported_configs}"
               IMPORTED_LOCATION "${_imgui_default_library}"
               IMPORTED_LOCATION_NOCONFIG "${_imgui_default_library}")
  # backend 所需 GLFW/OpenGL 经 imported target 传播，不由 UI 重复猜测。
  target_link_libraries(imgui-static INTERFACE 3rd_glfw OpenGL::GL)
endif()

if(NOT TARGET 3rd_imgui)
  # 业务模块始终链接 3rd_imgui，不感知来源；已有包装层不重复声明以支持嵌套。
  add_library(3rd_imgui INTERFACE)
  target_link_libraries(3rd_imgui INTERFACE imgui-static)
endif()

# imported target、全部配置和稳定包装层建立完成后才报告 package 可用。
set(imgui_FOUND TRUE)
