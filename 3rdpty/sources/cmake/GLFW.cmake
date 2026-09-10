set(GLFW_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/glfw")
if(NOT EXISTS "${GLFW_SOURCE_DIR}/CMakeLists.txt")
  message(
    FATAL_ERROR "缺少 GLFW 子模块。请执行 git submodule update --init --recursive。")
endif()

set(GLFW_BUILD_DOCS
    OFF
    CACHE BOOL "Disable GLFW documentation" FORCE)
set(GLFW_BUILD_TESTS
    OFF
    CACHE BOOL "Disable GLFW tests" FORCE)
set(GLFW_BUILD_EXAMPLES
    OFF
    CACHE BOOL "Disable GLFW examples" FORCE)
set(GLFW_INSTALL
    OFF
    CACHE BOOL "Disable GLFW installation" FORCE)

add_subdirectory("${GLFW_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/glfw_build"
                 EXCLUDE_FROM_ALL SYSTEM)

add_library(3rd_glfw INTERFACE)
target_link_libraries(3rd_glfw INTERFACE glfw)
