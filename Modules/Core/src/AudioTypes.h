#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AudioRoads::Core
{

/// @brief 描述端点对客户端而言可承担的音频流方向。
enum class DeviceFlow : std::uint8_t {
    /// 只能作为采集源。
    Input,
    /// 只能作为播放目标。
    Output,
    /// 可分别承担采集源或播放目标，不表示两个方向共享同一流格式。
    Duplex,
};

/// @brief 客户端用于展示和建立路由的稳定设备快照。
///
/// 平台后端负责把原生对象转换为这个值类型。快照不持有设备句柄，因而可以
/// 安全地从设备发现线程交给 UI；开始流时必须使用 id 重新打开原生端点。
/// id 只承诺在后端作用域内稳定，不承诺跨系统或重装后不变。
/// name 仅供展示，不得作为打开端点的唯一键。
/// 通道数与采样率是枚举时快照，打开流时仍需重新校验。
/// 类型不持有句柄、回调或线程，析构不会进入平台 API。
struct AudioDevice {
    /// @brief 后端作用域内稳定的设备标识符。
    std::string id;

    /// @brief 面向用户的设备名称。
    std::string name;

    /// @brief 产生此快照的平台后端名称。
    /// @note 只用于诊断和展示，业务选择应依赖带前缀的 id。
    std::string backend;

    /// @brief 设备支持的输入、输出或双工方向。
    DeviceFlow flow{ DeviceFlow::Input };

    /// @brief 当前原生格式报告的输入通道数。
    /// @note 零表示平台未报告，不能直接推断设备永久无此能力。
    std::uint32_t inputChannels{};

    /// @brief 当前原生格式报告的输出通道数。
    /// @note 零表示未知，后续打开流时仍应询问原生格式。
    std::uint32_t outputChannels{};

    /// @brief 当前原生格式的标称采样率，未知时为零。
    std::uint32_t sampleRate{};

    /// @brief 是否是此方向的系统默认设备。
    /// @note 该标志是展示快照，路由不得依赖它长期不变。
    bool isDefault{};
};

/// @brief 判断设备能否作为路由的采集源。
/// @note constexpr 实现使 UI 筛选与 Core 图约束共用同一语义。
[[nodiscard]] constexpr bool canCapture(const AudioDevice& device) noexcept
{
    return device.flow == DeviceFlow::Input ||
           device.flow == DeviceFlow::Duplex;
}

/// @brief 判断设备能否作为路由的播放终点。
/// @note Duplex 设备同时满足采集和播放方向。
[[nodiscard]] constexpr bool canRender(const AudioDevice& device) noexcept
{
    return device.flow == DeviceFlow::Output ||
           device.flow == DeviceFlow::Duplex;
}

}  // namespace AudioRoads::Core
