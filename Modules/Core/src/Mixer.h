#pragma once

#include <cstdint>
#include <expected>
#include <span>

namespace AudioRoads::Core
{

/// @brief 一块只读交错浮点音频及其路由参数。
/// @details 只保存 span 和标量，缓冲区所有权与生命周期均属于调用方。
struct MixInput {
    /// @brief 输入样本；长度必须与输出块一致。
    /// @note mixAudio 不执行重采样或通道映射。
    std::span<const float> samples;

    /// @brief 在线性域应用的块级增益。
    /// @invariant 必须是有限值且位于 0.0 到 4.0。
    float gain{ 1.0F };

    /// @brief 静音输入不参与累加。
    /// @note 静音是块级快照，不改变路由生命周期。
    bool muted{};
};

/// @brief 实时混音入口可能报告的契约错误。
enum class MixError : std::uint8_t {
    /// 至少一个输入块与输出块样本数不同。
    SizeMismatch,
    /// 至少一个增益不是有限值或超出允许范围。
    InvalidGain,
};

/// @brief 将多个等长音频块混合进调用方提供的缓冲区。
///
/// @warning 此函数运行在每个音频周期；禁止引入分配、锁、日志或系统调用。
/// 输出采用饱和限制，确保下游设备不会收到超出标准浮点范围的样本。
/// @details 先校验全部输入，失败时不修改输出；成功后清零、
/// 线性累加并在最后统一限幅到 [-1, 1]。
/// 所有输入必须属于同一时钟域，且与输出块等长。
/// 未来 SIMD 实现必须保持相同错误顺序、累加语义和限幅结果。
/// @return 成功时为空 expected，否则返回长度或增益契约错误。
[[nodiscard]] std::expected<void, MixError> mixAudio(
    std::span<const MixInput> inputs, std::span<float> output) noexcept;

}  // namespace AudioRoads::Core
