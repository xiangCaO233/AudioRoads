#pragma once

#include <atomic>
#include <cstddef>
#include <span>

namespace AudioRoads::Core
{

/// @brief 借用预分配存储的单生产者、单消费者浮点 PCM 环形缓冲区。
///
/// 生产者通常是来源采集回调，消费者通常是目标播放或虚拟麦克风回调。对象不
/// 拥有 storage，调用方必须保证其地址在缓冲区寿命内稳定，且不得与其他实例
/// 重叠写入。读写路径不分配、不加锁、不记录日志，也不调用平台 API。
/// @warning 严格限制为一个生产者线程和一个消费者线程；违反线程角色会产生
/// 数据竞争。析构或移动存储前必须先停止两侧回调。
///
/// 两个单调位置分别由其所属线程写入，对侧只以 acquire 读取；样本写完后才
/// release 发布写位置，样本读完后才 release 发布可复用槽位。缓冲区不会覆盖
/// 未消费数据，空间不足时由 write 返回部分数量，把丢弃/重试策略留给调用方。
/// readOrSilence 对欠载尾部补零，但只推进真实读取数量，从而保持来源时间线。
class SpscAudioRingBuffer final
{
public:
    /// @brief 使用调用方提供的连续样本区建立空缓冲区。
    /// @param storage 交错浮点 PCM 存储；空 span 合法，但所有写入都会返回零。
    explicit SpscAudioRingBuffer(std::span<float> storage) noexcept;

    SpscAudioRingBuffer(const SpscAudioRingBuffer&)            = delete;
    SpscAudioRingBuffer& operator=(const SpscAudioRingBuffer&) = delete;
    SpscAudioRingBuffer(SpscAudioRingBuffer&&)                 = delete;
    SpscAudioRingBuffer& operator=(SpscAudioRingBuffer&&)      = delete;

    /// @brief 尽可能写入输入样本，空间不足时保留尾部给下一周期处理。
    /// @return 实际写入的样本数，永远不超过 input.size()。
    /// @warning 只能由固定的单一生产者线程调用。
    /// @note 返回值小于输入长度表示溢出压力，函数本身不会阻塞等待消费者。
    [[nodiscard]] std::size_t write(std::span<const float> input) noexcept;

    /// @brief 读取可用样本，并把不足部分显式填充为静音。
    /// @return 从环形缓冲实际读取的样本数，不含补零部分。
    /// @warning 只能由固定的单一消费者线程调用。
    /// @note output 为空时不访问存储，也不改变读位置。
    [[nodiscard]] std::size_t readOrSilence(std::span<float> output) noexcept;

    /// @brief 返回底层存储最多容纳的样本数。
    [[nodiscard]] std::size_t capacity() const noexcept;

    /// @brief 返回调用瞬间可供消费者读取的近似样本数。
    /// @note 结果可能在返回后立刻变化，只适合遥测与流量控制提示。
    /// @warning 不得用该值先判断再假定下一次 readOrSilence 一定读到相同数量。
    [[nodiscard]] std::size_t readableSamples() const noexcept;

private:
    /// @brief 由调用方拥有且在实时处理期间地址稳定的样本存储。
    std::span<float> m_storage;

    /// @brief 消费者已经提交的单调样本位置。
    alignas(64) std::atomic_size_t m_readPosition{};

    /// @brief 生产者已经提交的单调样本位置。
    alignas(64) std::atomic_size_t m_writePosition{};
};

}  // namespace AudioRoads::Core
