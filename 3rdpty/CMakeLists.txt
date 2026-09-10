# SOURCES_BUILD 是显式依赖来源开关：ON 使用源码，OFF 只使用预编译包。
if(SOURCES_BUILD)
  add_subdirectory("sources")
else()
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
  find_package(glfw REQUIRED)
  find_package(imgui REQUIRED)
endif()
