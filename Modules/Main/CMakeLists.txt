if(APPLE)
  add_executable(AudioRoads MACOSX_BUNDLE src/Application.cpp src/main.cpp)
  set_target_properties(
    AudioRoads
    PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "AudioRoads"
               MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.xiangcao233.audioroads"
               MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
               MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
elseif(WIN32)
  add_executable(AudioRoads WIN32 src/Application.cpp src/main.cpp)
  if(MSVC)
    set_target_properties(AudioRoads PROPERTIES LINK_FLAGS
                                                "/ENTRY:mainCRTStartup")
  endif()
else()
  add_executable(AudioRoads src/Application.cpp src/main.cpp)
endif()

target_include_directories(AudioRoads PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(AudioRoads PRIVATE AudioRoadsAudio AudioRoadsUI
                                         Threads::Threads AudioRoadsWarnings)
