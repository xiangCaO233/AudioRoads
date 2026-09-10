# 每个依赖由独立脚本管理，并导出与预编译入口完全相同的 target。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include("GLFW")
include("imgui")
