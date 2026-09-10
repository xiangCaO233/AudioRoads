# 只在 SOURCES_BUILD=OFF 调用公共布局 helper；其限定路径查找阻止意外命中 系统 GLFW，并消费顶层已验证的
# PROJECT_LINKAGE。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 布局初始化必须早于任何头或库路径查找。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# GLFW 头文件不按 Debug/Release 拆分。
prebuilt_include_dir(_glfw_include_dir glfw)

if(NOT TARGET glfw)
  # UNKNOWN 不假定平台库后缀，GLOBAL 允许后续同级 UI 子目录复用此 target。
  add_library(glfw UNKNOWN IMPORTED GLOBAL)
  set_target_properties(glfw PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                        "${_glfw_include_dir}")
  prebuilt_target_configs(_glfw_configs)
  # 导入配置列表只收集已成功解析的产物。
  set(_glfw_imported_configs "")
  # 通用降级库在遍历首次命中时记录。
  set(_glfw_default_library "")
  foreach(_config IN LISTS _glfw_configs)
    # imported 属性名要求大写配置后缀。
    string(TOUPPER "${_config}" _config_upper)
    prebuilt_find_library(_library glfw "${_config}" glfw glfw3)
    list(APPEND _glfw_imported_configs "${_config_upper}")
    set_target_properties(glfw PROPERTIES "IMPORTED_LOCATION_${_config_upper}"
                                          "${_library}")
    # 首个成功配置同时作为 NOCONFIG 和通用位置，支持未声明配置的生成器。
    if(_glfw_default_library STREQUAL "")
      set(_glfw_default_library "${_library}")
    endif()
  endforeach()
  set_target_properties(
    glfw
    PROPERTIES IMPORTED_CONFIGURATIONS "${_glfw_imported_configs}"
               IMPORTED_LOCATION "${_glfw_default_library}"
               IMPORTED_LOCATION_NOCONFIG "${_glfw_default_library}")
  target_link_libraries(glfw INTERFACE OpenGL::GL Threads::Threads)
  # 系统依赖只在对应平台传播；业务模块仍只看到稳定 3rd_glfw 包装层。
  if(WIN32)
    # gdi32 是 GLFW Win32 后端的系统链接依赖。
    target_link_libraries(glfw INTERFACE gdi32)
  elseif(APPLE)
    # Apple frameworks 通过 find_library 获得可移植绝对引用。
    find_library(_glfw_cocoa Cocoa REQUIRED)
    find_library(_glfw_iokit IOKit REQUIRED)
    find_library(_glfw_quartz_core QuartzCore REQUIRED)
    find_library(_glfw_core_foundation CoreFoundation REQUIRED)
    target_link_libraries(
      glfw INTERFACE "${_glfw_cocoa}" "${_glfw_iokit}" "${_glfw_quartz_core}"
                     "${_glfw_core_foundation}")
  else()
    # Linux 预编译库约定自身处理 X11/Wayland，这里只补充通用 dl 与数学库。
    target_link_libraries(glfw INTERFACE ${CMAKE_DL_LIBS} m)
  endif()
endif()

if(NOT TARGET 3rd_glfw)
  # 包装层使业务 target 不感知源码/预编译来源，存在时不重复创建以支持嵌套。
  add_library(3rd_glfw INTERFACE)
  target_link_libraries(3rd_glfw INTERFACE glfw)
endif()

# 只有 imported target、配置映射、平台依赖和包装层全部建立后才报告找到。
set(glfw_FOUND TRUE)
