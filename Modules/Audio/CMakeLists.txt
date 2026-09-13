# 平台源变量只能得到一个定义，保证 IAudioBackend 工厂不会链接重复实现。
set(AUDIOROADS_AUDIO_PLATFORM_SOURCE "")

if(WIN32)
  # Windows 设备发现由 MMDevice/WASAPI 实现，平台库在下方同条件链接。
  set(AUDIOROADS_AUDIO_PLATFORM_SOURCE src/platform/WasapiBackend.cpp)
elseif(APPLE)
  # CoreAudio HAL 实现不在非 Apple 目标上参与语法检查或链接。
  set(AUDIOROADS_AUDIO_PLATFORM_SOURCE src/platform/CoreAudioBackend.cpp)
elseif(UNIX)
  # Linux 产品路径使用 PipeWire 0.3；开关关闭仅服务于 Core/UI 开发构建。
  if(AUDIOROADS_ENABLE_PIPEWIRE)
    # pkg-config 只为系统 PipeWire 开发包创建 imported target。
    find_package(PkgConfig REQUIRED)
    # REQUIRED 保证产品模式缺失 PipeWire 时不生成假后端。
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    set(AUDIOROADS_AUDIO_PLATFORM_SOURCE
        src/platform/PipeWireBackend.cpp src/platform/PipeWireRoutingEngine.cpp
        src/platform/PipeWireVirtualMicrophone.cpp)
  else()
    # 不可用后端会显式报错，禁止静默回退 ALSA 或伪造设备。
    set(AUDIOROADS_AUDIO_PLATFORM_SOURCE src/platform/UnavailableBackend.cpp)
  endif()
else()
  # 未接入平台在配置期失败，不能拖到工厂未定义或系统库缺失的链接阶段。
  message(FATAL_ERROR "AudioRoads 尚不支持当前平台。")
endif()

add_library(
  # 工厂与服务跨平台共用，变量只追加上方选中的后端翻译单元。
  # STATIC 表示客户端内部装配，不会由交付脚本安装为开发库。
  AudioRoadsAudio STATIC
  src/AudioBackendFactory.cpp src/AudioService.cpp
  ${AUDIOROADS_AUDIO_PLATFORM_SOURCE})
target_include_directories(AudioRoadsAudio
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# PUBLIC 只向仓库内 Main 传播 Core 和内部头路径；平台库及告警保持 PRIVATE。
target_link_libraries(
  AudioRoadsAudio
  PUBLIC AudioRoadsCore
  PRIVATE AudioRoadsWarnings)

if(WIN32)
  # ole32、propsys、uuid 仅满足 COM 设备枚举，不能进入公共使用接口。
  target_link_libraries(AudioRoadsAudio PRIVATE ole32 propsys uuid)
elseif(APPLE)
  # CoreAudio 提供 HAL，CoreFoundation 负责设备字符串值转换。
  find_library(AUDIOROADS_CORE_AUDIO CoreAudio REQUIRED)
  find_library(AUDIOROADS_CORE_FOUNDATION CoreFoundation REQUIRED)
  # framework 绝对引用仅进入当前实现 target，不向 Core 或最终调用者传播。
  target_link_libraries(AudioRoadsAudio PRIVATE "${AUDIOROADS_CORE_AUDIO}"
                                                "${AUDIOROADS_CORE_FOUNDATION}")
else()
  if(AUDIOROADS_ENABLE_PIPEWIRE)
    # 可用性宏与 imported target 在同一条件内，避免“编译了实现但未链接库”或 “工厂选择 PipeWire
    # 但实际编译不可用后端”的单边配置。
    target_link_libraries(AudioRoadsAudio PRIVATE PkgConfig::PIPEWIRE)
    target_compile_definitions(AudioRoadsAudio
                               PRIVATE AUDIOROADS_HAS_PIPEWIRE=1)
  endif()
endif()
