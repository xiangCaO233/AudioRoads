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
    // 定长数组使 span 的所有者和样本数在测试点可见，并覆盖整个混音调用。
    // 正负样本同时出现，防止实现只正确处理单一符号的累加或限幅。
    const std::array     first{ 0.5F, -0.5F, 0.75F, -0.75F };
    const std::array     second{ 0.75F, -0.75F, 0.75F, -0.75F };
    std::array<float, 4> output{};

    const std::array inputs{
        MixInput{ .samples = first, .gain = 1.0F, .muted = false },
        MixInput{ .samples = second, .gain = 1.0F, .muted = false },
    };
    // input span 全部借用栈数组，mixAudio 返回后没有任何缓冲区所有权需要释放。
    // 两路均为非静音、单位增益，结果只验证求和与最终限幅，不混入其他条件。
    assert(mixAudio(inputs, output).has_value());
    // 成功 expected 先确认契约成立，随后样本断言才具有数值意义。

    // 两端超出标准浮点范围时应在最终汇总后饱和。
    assert(output[0] == 1.0F);
    assert(output[1] == -1.0F);
    assert(output[2] == 1.0F);
    assert(output[3] == -1.0F);
    // 四项同时覆盖正向上限、负向下限和多个越界样本，结果应与输入顺序无关。

    const std::array shortInput{ 0.0F, 0.0F };
    // 长度契约失败时应返回明确错误，且不依赖设备、线程调度或随机数据。
    const std::array invalid{
        MixInput{ .samples = shortInput, .gain = 1.0F, .muted = false },
    };
    // output 仍使用四样本所有者，短输入明确制造 SizeMismatch 而非空缓冲特例。
    const auto result = mixAudio(invalid, output);
    // 校验在任何清零或累加之前完成，失败后 output 仍保持上一轮成功结果。
    // expected 必须携带精确错误枚举，不能把契约错误伪装为成功静音输出。
    assert(!result.has_value());
    assert(result.error() == MixError::SizeMismatch);
    // 错误类型稳定可判定，不依赖面向用户的文本或平台日志。
    return 0;
}
