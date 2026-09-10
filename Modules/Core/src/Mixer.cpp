#include "Mixer.h"

#include <algorithm>
#include <cmath>

namespace AudioRoads::Core
{

std::expected<void, MixError> mixAudio(std::span<const MixInput> inputs,
                                       std::span<float> output) noexcept
{
    // 先验证整块契约，失败时不留下只混合了一部分的输出。
    for ( const auto& input : inputs ) {
        if ( input.samples.size() != output.size() ) {
            return std::unexpected(MixError::SizeMismatch);
        }
        if ( !std::isfinite(input.gain) || input.gain < 0.0F ||
             input.gain > 4.0F ) {
            return std::unexpected(MixError::InvalidGain);
        }
    }

    std::ranges::fill(output, 0.0F);

    // MixInput 只携带 span 和标量；循环内没有所有权变更与隐藏分配。
    for ( const auto& input : inputs ) {
        if ( input.muted || input.gain == 0.0F ) continue;
        for ( std::size_t index = 0; index < output.size(); ++index ) {
            output[index] += input.samples[index] * input.gain;
        }
    }

    // 在汇总后统一限幅，避免路由顺序改变中间结果的非线性行为。
    for ( auto& sample : output ) {
        sample = std::clamp(sample, -1.0F, 1.0F);
    }
    return {};
}

}  // namespace AudioRoads::Core
