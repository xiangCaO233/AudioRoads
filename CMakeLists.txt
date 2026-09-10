cmake_minimum_required(VERSION 3.31)

set(AUDIOROADS_VERSION "0.1.0")

if(APPLE)
  project(
    AudioRoads
    VERSION ${AUDIOROADS_VERSION}
    LANGUAGES C CXX OBJC OBJCXX)
else()
  project(
    AudioRoads
    VERSION ${AUDIOROADS_VERSION}
    LANGUAGES C CXX)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      "${CMAKE_BUILD_TYPE}"
      CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release
                                               RelWithDebInfo MinSizeRel)
endif()

# C++23 是稳定基线；支持完整 C++26 模式的工具链可通过缓存变量显式切换。
set(AUDIOROADS_CXX_STANDARD
    "23"
    CACHE STRING "C++ language standard: 23 or 26.")
set_property(CACHE AUDIOROADS_CXX_STANDARD PROPERTY STRINGS 23 26)
if(NOT AUDIOROADS_CXX_STANDARD MATCHES "^(23|26)$")
  message(FATAL_ERROR "AUDIOROADS_CXX_STANDARD 只能是 23 或 26。")
endif()
set(CMAKE_CXX_STANDARD ${AUDIOROADS_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# 第三方依赖链接偏好与参照项目一致，仅接受 static 或 shared。
set(PROJECT_LINKAGE
    "static"
    CACHE STRING "Third-party dependency linkage preference.")
set_property(CACHE PROJECT_LINKAGE PROPERTY STRINGS static shared)
string(TOLOWER "${PROJECT_LINKAGE}" PROJECT_LINKAGE)
if(NOT PROJECT_LINKAGE MATCHES "^(static|shared)$")
  message(FATAL_ERROR "PROJECT_LINKAGE 只能是 static 或 shared。")
endif()

if(MSVC)
  if(PROJECT_LINKAGE STREQUAL "shared")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set(BUILD_SHARED_LIBS
        ON
        CACHE BOOL "Build shared third-party libraries." FORCE)
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(BUILD_SHARED_LIBS
        OFF
        CACHE BOOL "Build static third-party libraries." FORCE)
  endif()
elseif(PROJECT_LINKAGE STREQUAL "shared")
  set(BUILD_SHARED_LIBS
      ON
      CACHE BOOL "Build shared third-party libraries." FORCE)
else()
  set(BUILD_SHARED_LIBS
      OFF
      CACHE BOOL "Build static third-party libraries." FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# SOURCES_BUILD 是唯一的第三方来源开关，默认只允许使用预编译布局。
option(SOURCES_BUILD "Build third-party dependencies from sources." ON)
option(BUILD_TESTING "Build and register AudioRoads tests." OFF)
option(AUDIOROADS_ENABLE_PIPEWIRE
       "Enable the native PipeWire backend on Linux." ON)

if(BUILD_TESTING)
  enable_testing()
endif()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# 只把严格告警应用到自维护目标，避免为原样上游源码制造无关噪声。
add_library(AudioRoadsWarnings INTERFACE)
if(MSVC)
  target_compile_options(AudioRoadsWarnings INTERFACE /W4 /permissive-)
else()
  target_compile_options(AudioRoadsWarnings INTERFACE -Wall -Wextra -Wpedantic
                                                      -Wconversion -Wshadow)
endif()

add_subdirectory("3rdpty")
add_subdirectory("Modules")

include(GNUInstallDirs)
install(
  TARGETS AudioRoads
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  BUNDLE DESTINATION ".")
if(UNIX AND NOT APPLE)
  install(FILES "packaging/io.github.xiangcao233.audioroads.desktop"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
endif()

set(CPACK_PACKAGE_NAME "AudioRoads")
set(CPACK_PACKAGE_VENDOR "AudioRoads")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
elseif(WIN32)
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
