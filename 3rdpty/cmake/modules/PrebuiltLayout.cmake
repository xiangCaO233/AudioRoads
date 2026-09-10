# 初始化预编译根及平台、架构、工具链和编译器 ABI 标签。 所有布局键都是可覆盖 cache 变量，helper 只在调用方未定义时推导默认值。
# 共享头与平台二进制分离；本函数不创建具体第三方 target。
function(prebuilt_init default_root)
  # 预编译根允许调用方在命令行覆盖。
  set(PROJECT_PREBUILT_ROOT
      "${default_root}"
      CACHE PATH "Root directory containing prebuilt packages.")
  # cache 保留首次配置的用户覆盖；后续 helper 调用不会强行重写自定义镜像根。

  if(NOT DEFINED PROJECT_PREBUILT_PLATFORM)
    # 平台值未显式指定时才根据 CMake 目标平台推导。
    if(WIN32)
      # Windows 目录名使用小写，与产物上传路径一致。
      set(_platform windows)
    elseif(APPLE)
      # Apple 桌面目标统一映射到 macos 目录。
      set(_platform macos)
    else()
      # 其他当前支持目标都是现代 Linux。
      set(_platform linux)
    endif()
    set(PROJECT_PREBUILT_PLATFORM
        "${_platform}"
        CACHE STRING "Platform layout.")
    # 目录键表达目标平台而非构建主机，交叉编译时仍由 CMake target 变量决定。
  endif()

  if(NOT DEFINED PROJECT_PREBUILT_ARCH)
    # 架构推导只在用户未指定布局键时执行。
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
      # macOS Apple Silicon 包的稳定目录键为 arm64。
      set(_arch arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
      # 参考布局除 Apple Silicon 外的 64 位产物使用 x86_64 键。
      set(_arch x86_64)
    else()
      # 32 位降级键为 x86，具体工具链仍由下一层区分。
      set(_arch x86)
    endif()
    set(PROJECT_PREBUILT_ARCH
        "${_arch}"
        CACHE STRING "Architecture layout.")
    # 用户可覆盖特殊架构目录；默认推导只覆盖参考项目已有的三种稳定键。
  endif()

  if(NOT DEFINED PROJECT_PREBUILT_TOOLCHAIN)
    # 工具链键只描述 ABI 族，编译器版本放入后续 tag。
    if(MSVC)
      # MSVC 包不与 MinGW 或 clang-gnu 包混用。
      set(_toolchain msvc)
    elseif(MINGW)
      # MinGW 的具体发行形态由 compiler tag 继续区分。
      set(_toolchain mingw)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      # GNU 工具链使用 gcc 目录键。
      set(_toolchain gcc)
    else()
      # Clang 和其他当前支持的 LLVM 模式映射到 clang。
      set(_toolchain clang)
    endif()
    set(PROJECT_PREBUILT_TOOLCHAIN
        "${_toolchain}"
        CACHE STRING "Toolchain layout.")
    # 工具链族与 compiler tag 分层，避免把版本差异折叠进平台或架构目录。
  endif()

  if(NOT DEFINED PROJECT_PREBUILT_COMPILER_TAG)
    # 只提取主版本，避免补丁版本造成不必要目录分裂。
    string(REGEX MATCH "^[0-9]+" _compiler_major
                 "${CMAKE_CXX_COMPILER_VERSION}")
    if(MSVC)
      # MSVC/MinGW 固定标签遵循参考项目现有产物目录约定。
      set(_compiler_tag 2026)
    elseif(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      # MSYS2 Clang64 包使用 clang64 标签。
      set(_compiler_tag clang64)
    elseif(MINGW)
      # GCC MinGW 包使用 ucrt64 标签。
      set(_compiler_tag ucrt64)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      # 本地 GCC 标签包含主版本以区分 C++ ABI 差异。
      set(_compiler_tag "gcc${_compiler_major}")
    else()
      # 本地 Clang 标签同样包含主版本。
      set(_compiler_tag "clang${_compiler_major}")
    endif()
    set(PROJECT_PREBUILT_COMPILER_TAG
        "${_compiler_tag}"
        CACHE STRING "Compiler ABI layout.")
    # 标签进入最终库路径，是预编译 C++ ABI 匹配不可省略的一维。
  endif()

  set(_platform_root
      "${PROJECT_PREBUILT_ROOT}/binaries/${PROJECT_PREBUILT_PLATFORM}")
  # 平台根是后续查找的前置条件；缺失时不探测系统路径或其他依赖来源。
  if(NOT IS_DIRECTORY "${_platform_root}")
    message(
      FATAL_ERROR
        "SOURCES_BUILD=OFF 需要预编译库目录：${_platform_root}。请提供预编译库，或显式开启 SOURCES_BUILD。"
    )
  endif()
endfunction()

# 获取 headers/<package>/include 下跨配置共享的包头目录。 缺失目录代表预编译包不完整，不允许用系统头文件补齐二进制包。
function(prebuilt_include_dir out_var package)
  # 头文件布局不区分配置和链接形态。
  set(_include_dir "${PROJECT_PREBUILT_ROOT}/headers/${package}/include")
  # 缺失头文件时不允许继续查找二进制库。
  if(NOT IS_DIRECTORY "${_include_dir}")
    message(FATAL_ERROR "缺少预编译头文件目录：${_include_dir}")
  endif()
  set(${out_var}
      "${_include_dir}"
      PARENT_SCOPE)
endfunction()

# 建立配置候选顺序：Debug 始终保持独立 ABI，发布配置优先复用带符号产物。 输出使用 PARENT_SCOPE，使具体 Find
# 脚本不必复制大小写目录兼容规则。
function(prebuilt_config_candidates out_var config)
  # 输入配置大小写归一化后再建立候选列表。
  string(TOLOWER "${config}" _config_lower)
  if(_config_lower STREQUAL "debug")
    # Debug 绝不使用 Release 库降级。
    set(_candidates Debug debug)
  else()
    # 发布类配置优先使用带符号产物。
    set(_candidates RelWithDebInfo relwithdebinfo Release release)
  endif()
  set(${out_var}
      ${_candidates}
      PARENT_SCOPE)
endfunction()

# 在完整 ABI 根内解析指定包和库名，不允许落到宿主系统的同名库。 PROJECT_LINKAGE=shared 才追加 shared
# 层，静态布局保持参考项目的既有根。 NO_CACHE 避免不同 package 或重新配置时复用上一次 find_library 结果。
function(prebuilt_find_library out_var package config)
  # ABI 根包含所有必需维度，任一维不同都不复用库。
  set(_base
      "${PROJECT_PREBUILT_ROOT}/binaries/${PROJECT_PREBUILT_PLATFORM}/${package}/libs/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}/${PROJECT_PREBUILT_COMPILER_TAG}"
  )
  if(PROJECT_LINKAGE STREQUAL "shared")
    # 动态库放在额外 shared 层，静态库保持在 tag 根。
    string(APPEND _base "/shared")
  endif()

  prebuilt_config_candidates(_config_candidates "${config}")
  # helper 提供 Debug/发布隔离策略，具体 Find 脚本仍显式给出可接受库名。 候选目录按优先级顺序查找，首个命中即返回。
  foreach(_config_dir IN LISTS _config_candidates)
    find_library(
      _library
      NAMES ${ARGN}
      PATHS "${_base}/${_config_dir}"
      NO_DEFAULT_PATH NO_CACHE)
    if(_library)
      # 结果是已确认存在的绝对库路径。
      set(${out_var}
          "${_library}"
          PARENT_SCOPE)
      return()
    endif()
  endforeach()
  # 到达此处表示所有允许候选均失败，错误直接暴露完整 ABI 根便于补齐产物。
  message(FATAL_ERROR "缺少 ${package} 的 ${config} 预编译库：${_base}")
endfunction()

# 返回当前生成器需要绑定的 imported configurations。 多配置直接返回生成器列表；单配置使用当前类型，空类型明确降级
# RelWithDebInfo。
function(prebuilt_target_configs out_var)
  if(CMAKE_CONFIGURATION_TYPES)
    # Visual Studio 等多配置生成器一次声明全部配置。
    set(_configs ${CMAKE_CONFIGURATION_TYPES})
  elseif(CMAKE_BUILD_TYPE)
    # Ninja/Make 等单配置生成器只声明当前配置。
    set(_configs "${CMAKE_BUILD_TYPE}")
  else()
    # 未设置构建类型时使用可调试发布产物。
    set(_configs RelWithDebInfo)
  endif()
  set(${out_var}
      ${_configs}
      PARENT_SCOPE)
endfunction()
