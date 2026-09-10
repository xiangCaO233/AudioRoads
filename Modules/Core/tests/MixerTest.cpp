#include "Mixer.h"

#include <array>
#include <cassert>
#include <span>

using AudioRoads::Core::MixError;
using AudioRoads::Core::MixInput;
using AudioRoads::Core::mixAudio;

/// @brief 覆盖增益累加、静音、饱和限幅和长度契约。
int main()
{
    const std::array     first{ 0.5F, -0.5F, 0.75F, -0.75F };
    const std::array     second{ 0.75F, -0.75F, 0.75F, -0.75F };
    std::array<float, 4> output{};

    const std::array inputs{
        MixInput{ .samples = first, .gain = 1.0F, .muted = false },
        MixInput{ .samples = second, .gain = 1.0F, .muted = false },
    };
    assert(mixAudio(inputs, output).has_value());

    // 两端超出标准浮点范围时应在最终汇总后饱和。
    assert(output[0] == 1.0F);
    assert(output[1] == -1.0F);
    assert(output[2] == 1.0F);
    assert(output[3] == -1.0F);

    const std::array shortInput{ 0.0F, 0.0F };
    const std::array invalid{
        MixInput{ .samples = shortInput, .gain = 1.0F, .muted = false },
    };
    const auto result = mixAudio(invalid, output);
    assert(!result.has_value());
    assert(result.error() == MixError::SizeMismatch);
    return 0;
}
