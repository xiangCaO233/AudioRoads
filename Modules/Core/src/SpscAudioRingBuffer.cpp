#include "SpscAudioRingBuffer.h"

#include <algorithm>

namespace AudioRoads::Core
{

SpscAudioRingBuffer::SpscAudioRingBuffer(std::span<float> storage) noexcept
    : m_storage(storage)
{
    // 初始原子位置均为零，调用方存储中的旧值在第一次读取前不可见。
}

std::size_t SpscAudioRingBuffer::write(std::span<const float> input) noexcept
{
    const auto writePosition = m_writePosition.load(std::memory_order_relaxed);
    // acquire 与消费者的 release 配对，保证已经释放的槽位可以安全覆写。
    const auto readPosition = m_readPosition.load(std::memory_order_acquire);
    const auto usedSamples  = writePosition - readPosition;
    // 单生产者/单消费者约束保证 usedSamples 不会超过固定容量。
    const auto freeSamples  = m_storage.size() - usedSamples;
    const auto writeCount   = std::min(input.size(), freeSamples);

    // 单调位置只在索引存储时取模，跨越尾部无需复制或移动已有样本。
    for ( std::size_t offset = 0; offset < writeCount; ++offset ) {
        // writeCount 为零时循环不进入，因此空 storage 不会执行模零运算。
        m_storage[(writePosition + offset) % m_storage.size()] = input[offset];
    }

    // 样本全部写完后再发布位置，消费者不会观察到尚未完成的音频块。
    m_writePosition.store(writePosition + writeCount,
                          std::memory_order_release);
    return writeCount;
}

std::size_t SpscAudioRingBuffer::readOrSilence(std::span<float> output) noexcept
{
    const auto readPosition = m_readPosition.load(std::memory_order_relaxed);
    // acquire 与生产者的 release 配对，位置可见时对应样本也必须已经写完。
    const auto writePosition = m_writePosition.load(std::memory_order_acquire);
    const auto available     = writePosition - readPosition;
    // 只消费已由生产者 release 提交的前缀；尚在写入的尾部不会进入本周期。
    const auto readCount     = std::min(output.size(), available);

    for ( std::size_t offset = 0; offset < readCount; ++offset ) {
        // 与写侧使用相同单调位置取模，跨回绕仍保持原始样本顺序。
        output[offset] = m_storage[(readPosition + offset) % m_storage.size()];
    }
    // 目标设备必须持续得到完整周期；来源欠载时明确补零，不能重复旧缓冲内容。
    std::ranges::fill(output.subspan(readCount), 0.0F);

    // 只提交真实消费量，静音补位不凭空推进来源时间线。
    m_readPosition.store(readPosition + readCount, std::memory_order_release);
    return readCount;
}

std::size_t SpscAudioRingBuffer::capacity() const noexcept
{
    // 容量由外部 span 固定，流运行期间不得替换或缩放存储。
    return m_storage.size();
}

std::size_t SpscAudioRingBuffer::readableSamples() const noexcept
{
    // 两个位置独立采样，不承诺线性化快照；这里只提供控制面近似占用量。
    const auto writePosition = m_writePosition.load(std::memory_order_acquire);
    const auto readPosition  = m_readPosition.load(std::memory_order_acquire);
    // 第三方遥测线程可能跨过一次消费者提交读取到不一致快照，此时按零而非无符号
    // 下溢报告；生产者和消费者自身不依赖该近似值维持正确性。
    return writePosition >= readPosition ? writePosition - readPosition : 0;
}

}  // namespace AudioRoads::Core
