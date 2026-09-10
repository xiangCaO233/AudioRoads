#include "Mixer.h"

#include <algorithm>
#include <cmath>

namespace AudioRoads::Core
{

std::expected<void, MixError> mixAudio(std::span<const MixInput> inputs,
                                       std::span<float> output) noexcept
{
    // 先验证整块契约，失败时不留下只混合了一部分的输出。
    // 校验循环只读取 span 元数据与标量，不分配、不锁定，也不访问设备控制面。
    for ( const auto& input : inputs ) {
        if ( input.samples.size() != output.size() ) {
            // 任一路由长度不同都拒绝本周期，避免越界或尾部保留旧采样。
            return std::unexpected(MixError::SizeMismatch);
        }
        if ( !std::isfinite(input.gain) || input.gain < 0.0F ||
             input.gain > 4.0F ) {
            // NaN 会污染整块输出，Inf 会破坏限幅前累加，因此与越界值一起拒绝。
            return std::unexpected(MixError::InvalidGain);
        }
    }

    std::ranges::fill(output, 0.0F);

    // 成功路径从静音缓冲区开始，避免把上一音频周期的残留重复混入。
    // MixInput 只携带 span 和标量；循环内没有所有权变更与隐藏分配、锁或日志。
    for ( const auto& input : inputs ) {
        // 在进入内层样本循环前剔除无贡献输入，保持静音和零增益的快速路径。
        if ( input.muted || input.gain == 0.0F ) continue;
        for ( std::size_t index = 0; index < output.size(); ++index ) {
            // 所有 span 已统一验证长度，此处索引无需重复分支且不会越界。
            output[index] += input.samples[index] * input.gain;
        }
    }

    // 在汇总后统一限幅，避免路由顺序改变中间结果的非线性行为。
    for ( auto& sample : output ) {
        // 标准化到浮点 PCM 常用范围，保护下游转换但不改变合法样本。
        sample = std::clamp(sample, -1.0F, 1.0F);
    }
    return {};
}

}  // namespace AudioRoads::Core
