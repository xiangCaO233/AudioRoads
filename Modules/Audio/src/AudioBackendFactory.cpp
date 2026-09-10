#include "AudioBackendFactory.h"

#include <memory>

namespace AudioRoads::Audio
{

/// @brief Windows WASAPI 后端的构造入口。
[[nodiscard]] std::unique_ptr<IAudioBackend> createWasapiBackend();

/// @brief Linux PipeWire 后端的构造入口。
[[nodiscard]] std::unique_ptr<IAudioBackend> createPipeWireBackend();

/// @brief macOS CoreAudio 后端的构造入口。
[[nodiscard]] std::unique_ptr<IAudioBackend> createCoreAudioBackend();

/// @brief 无平台开发包时使用的显式不可用后端。
[[nodiscard]] std::unique_ptr<IAudioBackend> createUnavailableBackend();

std::unique_ptr<IAudioBackend> createPlatformAudioBackend()
{
#if defined(_WIN32)
    return createWasapiBackend();
#elif defined(__APPLE__)
    return createCoreAudioBackend();
#elif defined(AUDIOROADS_HAS_PIPEWIRE)
    return createPipeWireBackend();
#else
    return createUnavailableBackend();
#endif
}

}  // namespace AudioRoads::Audio
