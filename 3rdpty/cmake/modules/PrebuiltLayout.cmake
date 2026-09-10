# 预编译目录约定与 MusicMapMaker-Next 保持一致：共享头文件与平台二进制分离。
function(prebuilt_init default_root)
  set(PROJECT_PREBUILT_ROOT
      "${default_root}"
      CACHE PATH "Root directory containing prebuilt packages.")

  if(NOT DEFINED PROJECT_PREBUILT_PLATFORM)
    if(WIN32)
      set(_platform windows)
    elseif(APPLE)
      set(_platform macos)
    else()
      set(_platform linux)
    endif()
    set(PROJECT_PREBUILT_PLATFORM
        "${_platform}"
        CACHE STRING "Platform layout.")
  endif()

  if(NOT DEFINED PROJECT_PREBUILT_ARCH)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
      set(_arch arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_arch x86_64)
    else()
      set(_arch x86)
    endif()
    set(PROJECT_PREBUILT_ARCH
        "${_arch}"
        CACHE STRING "Architecture layout.")
  endif()

  if(NOT DEFINED PROJECT_PREBUILT_TOOLCHAIN)
    if(MSVC)
      set(_toolchain msvc)
    elseif(MINGW)
      set(_toolchain mingw)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      set(_toolchain gcc)
    else()
      set(_toolchain clang)
    endif()
    set(PROJECT_PREBUILT_TOOLCHAIN
        "${_toolchain}"
        CACHE STRING "Toolchain layout.")
  endif()

  if(NOT DEFINED PROJECT_PREBUILT_COMPILER_TAG)
    string(REGEX MATCH "^[0-9]+" _compiler_major
                 "${CMAKE_CXX_COMPILER_VERSION}")
    if(MSVC)
      set(_compiler_tag 2026)
    elseif(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set(_compiler_tag clang64)
    elseif(MINGW)
      set(_compiler_tag ucrt64)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      set(_compiler_tag "gcc${_compiler_major}")
    else()
      set(_compiler_tag "clang${_compiler_major}")
    endif()
    set(PROJECT_PREBUILT_COMPILER_TAG
        "${_compiler_tag}"
        CACHE STRING "Compiler ABI layout.")
  endif()

  set(_platform_root
      "${PROJECT_PREBUILT_ROOT}/binaries/${PROJECT_PREBUILT_PLATFORM}")
  if(NOT IS_DIRECTORY "${_platform_root}")
    message(
      FATAL_ERROR
        "SOURCES_BUILD=OFF 需要预编译库目录：${_platform_root}。请提供预编译库，或显式开启 SOURCES_BUILD。"
    )
  endif()
endfunction()

# 获取指定包的共享头文件目录。
function(prebuilt_include_dir out_var package)
  set(_include_dir "${PROJECT_PREBUILT_ROOT}/headers/${package}/include")
  if(NOT IS_DIRECTORY "${_include_dir}")
    message(FATAL_ERROR "缺少预编译头文件目录：${_include_dir}")
  endif()
  set(${out_var}
      "${_include_dir}"
      PARENT_SCOPE)
endfunction()

# 发布配置优先复用带符号的 RelWithDebInfo，Debug 始终保持独立 ABI。
function(prebuilt_config_candidates out_var config)
  string(TOLOWER "${config}" _config_lower)
  if(_config_lower STREQUAL "debug")
    set(_candidates Debug debug)
  else()
    set(_candidates RelWithDebInfo relwithdebinfo Release release)
  endif()
  set(${out_var}
      ${_candidates}
      PARENT_SCOPE)
endfunction()

# 每个 Find 脚本显式指定包名和库名，此 helper 只负责解析公共目录层级。
function(prebuilt_find_library out_var package config)
  set(_base
      "${PROJECT_PREBUILT_ROOT}/binaries/${PROJECT_PREBUILT_PLATFORM}/${package}/libs/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}/${PROJECT_PREBUILT_COMPILER_TAG}"
  )
  if(PROJECT_LINKAGE STREQUAL "shared")
    string(APPEND _base "/shared")
  endif()

  prebuilt_config_candidates(_config_candidates "${config}")
  foreach(_config_dir IN LISTS _config_candidates)
    find_library(
      _library
      NAMES ${ARGN}
      PATHS "${_base}/${_config_dir}"
      NO_DEFAULT_PATH NO_CACHE)
    if(_library)
      set(${out_var}
          "${_library}"
          PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "缺少 ${package} 的 ${config} 预编译库：${_base}")
endfunction()

# 单配置与多配置生成器都通过显式 IMPORTED_CONFIGURATIONS 绑定正确产物。
function(prebuilt_target_configs out_var)
  if(CMAKE_CONFIGURATION_TYPES)
    set(_configs ${CMAKE_CONFIGURATION_TYPES})
  elseif(CMAKE_BUILD_TYPE)
    set(_configs "${CMAKE_BUILD_TYPE}")
  else()
    set(_configs RelWithDebInfo)
  endif()
  set(${out_var}
      ${_configs}
      PARENT_SCOPE)
endfunction()
