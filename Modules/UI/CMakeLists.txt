add_library(AudioRoadsUI STATIC src/AppWindow.cpp src/MainView.cpp)
target_include_directories(AudioRoadsUI
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  AudioRoadsUI
  PUBLIC AudioRoadsCore
  PRIVATE 3rd_imgui OpenGL::GL AudioRoadsWarnings)
