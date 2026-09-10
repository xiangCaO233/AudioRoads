set(AUDIOROADS_SYSTEM_FONT_SOURCE "")
if(WIN32)
  set(AUDIOROADS_SYSTEM_FONT_SOURCE src/SystemFont-windows.cpp)
elseif(APPLE)
  set(AUDIOROADS_SYSTEM_FONT_SOURCE src/SystemFont-macos.cpp)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
  set(AUDIOROADS_SYSTEM_FONT_SOURCE src/SystemFont-linux.cpp)
endif()

add_library(AudioRoadsUI STATIC src/AppWindow.cpp src/MainView.cpp
                                ${AUDIOROADS_SYSTEM_FONT_SOURCE})
target_include_directories(AudioRoadsUI
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  AudioRoadsUI
  PUBLIC AudioRoadsCore
  PRIVATE 3rd_imgui OpenGL::GL AudioRoadsWarnings)

if(WIN32)
  target_link_libraries(AudioRoadsUI PRIVATE dwrite user32)
elseif(APPLE)
  find_library(AUDIOROADS_CORE_TEXT CoreText REQUIRED)
  find_library(AUDIOROADS_CORE_FOUNDATION CoreFoundation REQUIRED)
  target_link_libraries(AudioRoadsUI PRIVATE "${AUDIOROADS_CORE_TEXT}"
                                             "${AUDIOROADS_CORE_FOUNDATION}")
else()
  target_link_libraries(AudioRoadsUI PRIVATE PkgConfig::FONTCONFIG)
endif()

if(BUILD_TESTING)
  add_executable(SystemFontTest tests/SystemFontTest.cpp
                                ${AUDIOROADS_SYSTEM_FONT_SOURCE})
  target_include_directories(SystemFontTest
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(SystemFontTest PRIVATE AudioRoadsWarnings)
  if(WIN32)
    target_link_libraries(SystemFontTest PRIVATE dwrite user32)
  elseif(APPLE)
    target_link_libraries(
      SystemFontTest PRIVATE "${AUDIOROADS_CORE_TEXT}"
                             "${AUDIOROADS_CORE_FOUNDATION}")
  else()
    target_link_libraries(SystemFontTest PRIVATE PkgConfig::FONTCONFIG)
  endif()
  add_test(NAME SystemFontTest COMMAND SystemFontTest)
endif()
