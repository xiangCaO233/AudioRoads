#pragma once

#include "IAudioBackend.h"

#include <memory>

namespace AudioRoads::Audio
{

/// @brief 为编译目标创建唯一的平台原生音频后端。
///
/// 实际类型由编译期平台宏决定，不执行运行期插件发现。
/// Windows、Linux 和 macOS 各自的实现仅在相应目标中编译。
/// Linux 显式关闭 PipeWire 时返回可报错的不可用后端，
/// 不伪造设备，使 UI 能呈现真实构建状态。
/// 工厂本身不枚举设备、不创建流也不启动线程。
/// 返回的 unique_ptr 由 AudioService 立即接管，不存在共享所有权。
/// 工厂不抛出异常；后端初始化故障由后续操作明确报告。
/// 调用方因此不需要包含任何平台音频头文件。
/// @return 当前编译目标的 `IAudioBackend` 唯一所有者。
[[nodiscard]] std::unique_ptr<IAudioBackend> createPlatformAudioBackend();

}  // namespace AudioRoads::Audio
