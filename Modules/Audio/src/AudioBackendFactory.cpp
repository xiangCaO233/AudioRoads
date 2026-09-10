#include "AudioBackendFactory.h"

#include <memory>

namespace AudioRoads::Audio
{

/// @brief Windows WASAPI 后端的构造入口。
/// @return 新建接口所有者；具体类型只在 Windows 翻译单元中可见。
[[nodiscard]] std::unique_ptr<IAudioBackend> createWasapiBackend();

/// @brief Linux PipeWire 后端的构造入口。
/// @return 新建接口所有者；只有链接 PipeWire 时该符号才进入目标。
[[nodiscard]] std::unique_ptr<IAudioBackend> createPipeWireBackend();

/// @brief macOS CoreAudio 后端的构造入口。
/// @return 新建接口所有者；具体 HAL 类型不会传播到服务层。
[[nodiscard]] std::unique_ptr<IAudioBackend> createCoreAudioBackend();

/// @brief 显式关闭 Linux PipeWire 时使用的可观测不可用后端。
/// @return 行为稳定的错误后端，不伪造设备或静默选择其他 API。
[[nodiscard]] std::unique_ptr<IAudioBackend> createUnavailableBackend();

std::unique_ptr<IAudioBackend> createPlatformAudioBackend()
{
    // 工厂仅在 AudioService 构造期运行，不属于设备枚举或实时音频路径。
    // 平台选择保持在编译期：未选中的原生库不会被链接，因此不得用运行时探测
    // 静默切换实现。Linux 的不可用后端仅服务于显式关闭 PipeWire 的开发构建。
#if defined(_WIN32)
    // CMake 同一分支只编译 WasapiBackend.cpp，确保引用与链接实现一致。
    return createWasapiBackend();
#elif defined(__APPLE__)
    // Apple 分支的 CoreAudio frameworks 与此工厂选择同步配置。
    return createCoreAudioBackend();
#elif defined(AUDIOROADS_HAS_PIPEWIRE)
    // 宏仅在 pkg-config target 成功链接时定义，不单凭 __linux__ 猜测能力。
    return createPipeWireBackend();
#else
    // 唯一允许的降级是用户显式关闭 PipeWire 后的开发态错误后端。
    // 未知操作系统已由 CMake 配置期拒绝，因此这里不承担通用平台 fallback。
    return createUnavailableBackend();
#endif
}

}  // namespace AudioRoads::Audio
