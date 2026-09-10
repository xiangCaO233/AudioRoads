#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AudioRoads::Core
{

/// @brief 描述端点对客户端而言可承担的音频流方向。
enum class DeviceFlow : std::uint8_t {
    Input,
    Output,
    Duplex,
};

/// @brief 客户端用于展示和建立路由的稳定设备快照。
///
/// 平台后端负责把原生对象转换为这个值类型。快照不持有设备句柄，因而可以
/// 安全地从设备发现线程交给 UI；开始流时必须使用 id 重新打开原生端点。
struct AudioDevice {
    /// @brief 后端作用域内稳定的设备标识符。
    std::string id;

    /// @brief 面向用户的设备名称。
    std::string name;

    /// @brief 产生此快照的平台后端名称。
    std::string backend;

    /// @brief 设备支持的输入、输出或双工方向。
    DeviceFlow flow{ DeviceFlow::Input };

    /// @brief 当前原生格式报告的输入通道数。
    std::uint32_t inputChannels{};

    /// @brief 当前原生格式报告的输出通道数。
    std::uint32_t outputChannels{};

    /// @brief 当前原生格式的标称采样率，未知时为零。
    std::uint32_t sampleRate{};

    /// @brief 是否是此方向的系统默认设备。
    bool isDefault{};
};

/// @brief 判断设备能否作为路由的采集源。
[[nodiscard]] constexpr bool canCapture(const AudioDevice& device) noexcept
{
    return device.flow == DeviceFlow::Input ||
           device.flow == DeviceFlow::Duplex;
}

/// @brief 判断设备能否作为路由的播放终点。
[[nodiscard]] constexpr bool canRender(const AudioDevice& device) noexcept
{
    return device.flow == DeviceFlow::Output ||
           device.flow == DeviceFlow::Duplex;
}

}  // namespace AudioRoads::Core
