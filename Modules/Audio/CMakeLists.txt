set(AUDIOROADS_AUDIO_PLATFORM_SOURCE "")

if(WIN32)
  set(AUDIOROADS_AUDIO_PLATFORM_SOURCE src/platform/WasapiBackend.cpp)
elseif(APPLE)
  set(AUDIOROADS_AUDIO_PLATFORM_SOURCE src/platform/CoreAudioBackend.cpp)
elseif(UNIX)
  if(AUDIOROADS_ENABLE_PIPEWIRE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    set(AUDIOROADS_AUDIO_PLATFORM_SOURCE src/platform/PipeWireBackend.cpp)
  else()
    # 该分支只供缺少 PipeWire 开发包的 Core/UI 开发环境使用。
    set(AUDIOROADS_AUDIO_PLATFORM_SOURCE src/platform/UnavailableBackend.cpp)
  endif()
else()
  message(FATAL_ERROR "AudioRoads 尚不支持当前平台。")
endif()

add_library(
  AudioRoadsAudio STATIC src/AudioBackendFactory.cpp src/AudioService.cpp
                         ${AUDIOROADS_AUDIO_PLATFORM_SOURCE})
target_include_directories(AudioRoadsAudio
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  AudioRoadsAudio
  PUBLIC AudioRoadsCore
  PRIVATE AudioRoadsWarnings)

if(WIN32)
  target_link_libraries(AudioRoadsAudio PRIVATE ole32 propsys uuid)
elseif(APPLE)
  find_library(AUDIOROADS_CORE_AUDIO CoreAudio REQUIRED)
  find_library(AUDIOROADS_CORE_FOUNDATION CoreFoundation REQUIRED)
  target_link_libraries(AudioRoadsAudio PRIVATE "${AUDIOROADS_CORE_AUDIO}"
                                                "${AUDIOROADS_CORE_FOUNDATION}")
else()
  if(AUDIOROADS_ENABLE_PIPEWIRE)
    target_link_libraries(AudioRoadsAudio PRIVATE PkgConfig::PIPEWIRE)
    target_compile_definitions(AudioRoadsAudio
                               PRIVATE AUDIOROADS_HAS_PIPEWIRE=1)
  endif()
endif()
