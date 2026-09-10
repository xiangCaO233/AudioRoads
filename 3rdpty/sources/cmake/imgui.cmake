set(IMGUI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui")
if(NOT EXISTS "${IMGUI_SOURCE_DIR}/imgui.cpp")
  message(
    FATAL_ERROR "缺少 ImGui 子模块。请执行 git submodule update --init --recursive。")
endif()

# 客户端只编译官方 GLFW + OpenGL3 后端，平台音频 API 不进入渲染层。
set(IMGUI_SOURCES
    "${IMGUI_SOURCE_DIR}/imgui.cpp"
    "${IMGUI_SOURCE_DIR}/imgui_draw.cpp"
    "${IMGUI_SOURCE_DIR}/imgui_tables.cpp"
    "${IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
    "${IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")

set(IMGUI_LIBRARY_TYPE STATIC)
if(PROJECT_LINKAGE STREQUAL "shared")
  set(IMGUI_LIBRARY_TYPE SHARED)
endif()

add_library(imgui-static ${IMGUI_LIBRARY_TYPE} ${IMGUI_SOURCES})
target_include_directories(imgui-static PUBLIC "${IMGUI_SOURCE_DIR}"
                                               "${IMGUI_SOURCE_DIR}/backends")
target_compile_features(imgui-static PRIVATE cxx_std_11)
target_link_libraries(imgui-static PUBLIC 3rd_glfw OpenGL::GL)

if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
  set_target_properties(imgui-static PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON
                                                OUTPUT_NAME imgui)
  target_compile_definitions(
    imgui-static
    PRIVATE "IMGUI_API=__declspec(dllexport)"
            "IMGUI_IMPL_API=__declspec(dllexport)"
    INTERFACE "IMGUI_API=__declspec(dllimport)"
              "IMGUI_IMPL_API=__declspec(dllimport)")
endif()

add_library(3rd_imgui INTERFACE)
target_link_libraries(3rd_imgui INTERFACE imgui-static)
