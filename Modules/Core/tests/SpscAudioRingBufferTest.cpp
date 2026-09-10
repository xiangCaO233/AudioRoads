#include "SpscAudioRingBuffer.h"

#include <array>
#include <cassert>

using AudioRoads::Core::SpscAudioRingBuffer;

/// @brief 验证容量边界、回绕顺序和欠载补零契约。
int main()
{
    std::array<float, 4> storage{};
    SpscAudioRingBuffer  buffer{ storage };

    const std::array first{ 1.0F, 2.0F, 3.0F };
    // 首次写入不跨界，剩余容量应准确反映一个槽位。
    assert(buffer.capacity() == 4);
    assert(buffer.write(first) == first.size());
    assert(buffer.readableSamples() == first.size());

    std::array<float, 2> firstRead{};
    // 消费两个样本后只推进读位置，不移动剩余样本或重置写位置。
    assert(buffer.readOrSilence(firstRead) == firstRead.size());
    assert((firstRead == std::array{ 1.0F, 2.0F }));

    const std::array wrapped{ 4.0F, 5.0F, 6.0F, 7.0F };
    // 此时只有三个空槽，部分写入必须保留最早数据且拒绝输入尾部。
    assert(buffer.write(wrapped) == 3);
    // 满缓冲允许占用全部四个槽位，读写位置差而不是空槽哨兵区分满和空。
    assert(buffer.readableSamples() == storage.size());

    std::array<float, 5> secondRead{};
    // 四个真实样本跨越物理尾部，第五项由欠载策略补成静音。
    assert(buffer.readOrSilence(secondRead) == 4);
    // 读取顺序必须是旧尾部 3，再接新写入的 4/5/6；被拒绝的 7 不得出现。
    assert((secondRead == std::array{ 3.0F, 4.0F, 5.0F, 6.0F, 0.0F }));
    assert(buffer.readableSamples() == 0);

    std::array<float, 2> silence{ -1.0F, -1.0F };
    // 完全欠载仍要覆盖整个目标周期，防止下游重复播放上一块残留。
    assert(buffer.readOrSilence(silence) == 0);
    // 返回值区分真实静音样本和欠载补零，调用方可据此记录非实时遥测。
    assert((silence == std::array{ 0.0F, 0.0F }));
    return 0;
}
