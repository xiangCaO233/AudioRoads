#pragma once

#include <cstdint>
#include <expected>
#include <span>

namespace AudioRoads::Core
{

/// @brief 一块只读交错浮点音频及其路由参数。
struct MixInput {
    /// @brief 输入样本；长度必须与输出块一致。
    std::span<const float> samples;

    /// @brief 在线性域应用的块级增益。
    float gain{ 1.0F };

    /// @brief 静音输入不参与累加。
    bool muted{};
};

/// @brief 实时混音入口可能报告的契约错误。
enum class MixError : std::uint8_t {
    SizeMismatch,
    InvalidGain,
};

/// @brief 将多个等长音频块混合进调用方提供的缓冲区。
///
/// @warning 此函数运行在每个音频周期；禁止引入分配、锁、日志或系统调用。
/// 输出采用饱和限制，确保下游设备不会收到超出标准浮点范围的样本。
[[nodiscard]] std::expected<void, MixError> mixAudio(
    std::span<const MixInput> inputs, std::span<float> output) noexcept;

}  // namespace AudioRoads::Core
