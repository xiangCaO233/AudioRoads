#pragma once

#include "IAudioBackend.h"

#include <memory>

namespace AudioRoads::Audio
{

/// @brief 为编译目标创建唯一的平台原生音频后端。
[[nodiscard]] std::unique_ptr<IAudioBackend> createPlatformAudioBackend();

}  // namespace AudioRoads::Audio
