#include "PipeWireRoutingEngine.h"

#include "Mixer.h"
#include "SpscAudioRingBuffer.h"

#include <pipewire/keys.h>
#include <pipewire/pipewire.h>

#include <spa/buffer/buffer.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AudioRoads::Audio
{
namespace
{

// PipeWire 负责把设备原生格式转换到统一引擎域；固定内部格式可让 Core 混音器
// 保持无平台分支，也使不同设备时钟通过 PipeWire adapter 进入相同标称速率。
constexpr std::uint32_t ROUTING_SAMPLE_RATE = 48'000U;
// 首阶段统一使用常见的左右双声道；单声道麦克风由会话管理器复制/映射到两路。
constexpr std::uint32_t ROUTING_CHANNELS = 2U;
// 字节常量只用于校验 SPA 缓冲边界和填写 chunk，不参与格式协商选择。
constexpr std::size_t BYTES_PER_SAMPLE = sizeof(float);
constexpr std::size_t BYTES_PER_FRAME  = BYTES_PER_SAMPLE * ROUTING_CHANNELS;
// 上限覆盖本机 PipeWire quantum-limit；异常超大请求会在回调中截断到已分配区。
constexpr std::size_t MAX_BLOCK_FRAMES  = 8'192U;
constexpr std::size_t MAX_BLOCK_SAMPLES = MAX_BLOCK_FRAMES * ROUTING_CHANNELS;
// 每条边保留约 250 ms 数据，用非阻塞丢弃换取设备瞬时调度抖动下的连续播放。
// 跨时钟域长期漂移的占用量反馈重采样仍属于后续阶段，不能在回调里 sleep 对齐。
constexpr std::size_t RING_BUFFER_FRAMES = 12'000U;
constexpr std::size_t RING_BUFFER_SAMPLES =
    RING_BUFFER_FRAMES * ROUTING_CHANNELS;
// Core ID 的类别前缀同时承担类型校验，避免错误地把来源 serial 用作播放目标。
constexpr std::string_view DEVICE_INPUT_PREFIX  = "pipewire:device-input:";
constexpr std::string_view DEVICE_OUTPUT_PREFIX = "pipewire:device-output:";

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "实时路由参数要求无锁 32 位原子");

/// @brief 一条 Core 路由解析到某个原生来源节点后的执行拓扑项。
/// @details 聚合应用若有多个活动节点，会为同一 RouteId 产生多个项并共同混入
/// 一个目标。字符串只在控制面构建和比较，实时回调不访问它们。
struct NativeRoute {
    /// @brief 用户路由身份；参数更新时用于与 Core 路由保持对应。
    Core::RouteId routeId{};

    /// @brief 本轮 capture stream 的 target.object，通常为 object.serial。
    std::string sourceObject;

    /// @brief 本轮 playback stream 的 target.object，通常为 object.serial。
    std::string targetObject;

    /// @brief 创建工作区时取得的初值，运行后通过无锁原子发布更新。
    float gain{ 1.0F };

    /// @brief 创建工作区时取得的静音初值，不影响流及缓冲生命周期。
    bool muted{};

    /// @brief 判断能否保留当前平台流，仅原生端点关系参与比较。
    /// @details gain/muted 改变不需要停流；它们通过 RouteRuntime 单独更新。
    [[nodiscard]] bool sameTopology(const NativeRoute& other) const noexcept
    {
        return routeId == other.routeId && sourceObject == other.sourceObject &&
               targetObject == other.targetObject;
    }
};

/// @brief 提取带类别前缀端点 ID 中可交给 target.object 的 serial。
/// @param endpointId Core 保存的后端作用域稳定 ID。
/// @param prefix 调用路径要求的来源或目标类别前缀。
/// @return 类别匹配且后缀非空时返回借用视图，否则为空。
[[nodiscard]] std::string_view nativeObject(std::string_view endpointId,
                                            std::string_view prefix) noexcept
{
    if ( !endpointId.starts_with(prefix) ) return {};
    endpointId.remove_prefix(prefix.size());
    return endpointId;
}

/// @brief 为平台控制面错误保留统一操作上下文。
/// @details message 只在建流控制路径构造，实时回调不会调用本函数。
/// @return 拥有文本的值对象，可安全移交到 AudioService 和 UI。
[[nodiscard]] AudioBackendError routingError(std::string message)
{
    return AudioBackendError{ .operation = "同步 PipeWire 音频路由",
                              .message   = std::move(message) };
}

/// @brief 每个原生来源到单个目标消费者之间的独立 SPSC 数据通道。
/// @details storage、scratch 在流启动前完成分配，地址在所有回调停止前稳定。
/// 来源扇出时每条边各建一个实例，从而每个 ringBuffer 始终只有一个目标回调
/// 消费；应用聚合成多个原生流时也按原生流拆边，避免多生产者写同一 SPSC。
struct RouteRuntime {
    /// @brief 分配固定容量工作区并保存初始实时参数。
    /// @param route 已完成稳定 ID 解析的执行项；构造期间平台线程尚未启动。
    explicit RouteRuntime(const NativeRoute& route)
        : routeId(route.routeId)
        , storage(RING_BUFFER_SAMPLES)
        , scratch(MAX_BLOCK_SAMPLES)
        , ringBuffer(storage)
        , gainBits(std::bit_cast<std::uint32_t>(route.gain))
        , mutedValue(route.muted ? 1U : 0U)
    {
        // 构造完成后才会把地址放入来源/目标回调列表，避免观察半初始化工作区。
    }

    /// @brief 控制面以无锁原子发布新的增益和静音值。
    void updateParameters(float gain, bool muted) noexcept
    {
        gainBits.store(std::bit_cast<std::uint32_t>(gain),
                       std::memory_order_release);
        mutedValue.store(muted ? 1U : 0U, std::memory_order_release);
    }

    /// @brief 在目标回调周期边界取得最近发布的线性增益。
    [[nodiscard]] float gain() const noexcept
    {
        return std::bit_cast<float>(gainBits.load(std::memory_order_acquire));
    }

    /// @brief 在目标回调周期边界取得最近发布的静音标记。
    [[nodiscard]] bool muted() const noexcept
    {
        return mutedValue.load(std::memory_order_acquire) != 0U;
    }

    /// @brief 原始用户路由 ID；聚合应用拆分后允许多个实例持有相同值。
    Core::RouteId routeId{};

    /// @brief SPSC 借用的固定样本所有者；必须声明在 ringBuffer 之前。
    std::vector<float> storage;

    /// @brief 目标回调读取单条入边的固定最大块，随后由 MixInput 借用。
    std::vector<float> scratch;

    /// @brief 来源回调与一个目标回调之间唯一的数据交换对象。
    Core::SpscAudioRingBuffer ringBuffer;

    /// @brief float 的位模式；uint32_t 原子有编译期无锁保证。
    std::atomic<std::uint32_t> gainBits;

    /// @brief 使用同一种无锁原子宽度，零为未静音，非零为静音。
    std::atomic<std::uint32_t> mutedValue;
};

/// @brief 控制线程与状态回调共享的单流基础状态。
/// @details process 回调不读 error；字符串分配只发生在 PipeWire 控制循环。
struct StreamRuntime {
    /// @brief 状态回调用于唤醒同步等待的线程循环；不归本结构所有。
    pw_thread_loop* loop{};

    /// @brief 由实现统一销毁的流句柄；实时回调存在期间非空且稳定。
    pw_stream* stream{};

    /// @brief 只在持有 thread-loop 锁的状态回调和同步等待路径访问。
    pw_stream_state state{ PW_STREAM_STATE_UNCONNECTED };

    /// @brief 异步错误文本；实时 process 回调不读写该字符串。
    std::string error;
};

/// @brief 一个原生来源流及其扇出的独立路由缓冲。
/// @details 同一物理麦克风只打开一次；它的一个回调可以顺序写入多条独立边。
struct SourceRuntime : StreamRuntime {
    /// @brief 当前快照内用于定向自动连接的 serial/name。
    std::string targetObject;

    /// @brief 非拥有指针；RouteRuntime 比所有来源和目标流活得更久。
    std::vector<RouteRuntime*> routes;
};

/// @brief 一个原生播放目标及其所有入边的预分配混音工作区。
/// @details 每个目标只有一个播放流，因此多路求和、限幅只发生一次且顺序稳定。
struct TargetRuntime : StreamRuntime {
    /// @brief 当前快照内用于定向自动连接的 serial/name。
    std::string targetObject;

    /// @brief 所有进入该目标的边；顺序在本轮拓扑寿命内固定。
    std::vector<RouteRuntime*> routes;

    /// @brief 与 routes 等长，回调只改写元素，不改变 vector 容量。
    std::vector<Core::MixInput> mixInputs;
};

/// @brief 回收捕获缓冲前，把有效交错 float32 样本扇出到每条 SPSC。
/// @param userData SourceRuntime 的稳定地址，由 pw_stream 在建流时借用。
/// @warning 由 PipeWire 实时线程调用，不得分配、锁、日志或阻塞。
/// @details 无可用缓冲时直接返回，让下一周期继续；取得缓冲后所有分支都必须
/// 重新 queue，避免耗尽 PipeWire 缓冲池。SPA chunk 的 offset/size 均来自外部，
/// 读取前必须夹到 data.maxsize，尾部不足一个 float 的字节不进入 PCM span。
void processSource(void* userData)
{
    // stream 在 stop 持有控制锁销毁之前保持稳定，回调不复制平台所有权。
    auto& runtime = *static_cast<SourceRuntime*>(userData);
    auto* buffer  = pw_stream_dequeue_buffer(runtime.stream);
    // 暂时没有可消费缓冲不是致命错误，实时路径也不能构造诊断字符串。
    if ( buffer == nullptr ) return;

    auto* spaBuffer = buffer->buffer;
    // 当前协商是交错格式，只消费第一个 data block；零 block 仍需归还缓冲。
    if ( spaBuffer != nullptr && spaBuffer->n_datas > 0U ) {
        auto& data = spaBuffer->datas[0];
        // MAP_BUFFERS 通常提供 data/chunk，但导入型或损坏缓冲必须安全跳过。
        if ( data.data != nullptr && data.chunk != nullptr &&
             data.chunk->offset <= data.maxsize ) {
            // 服务端 chunk
            // 不可信任为天然落在映射范围内，先计算剩余边界再取小值。
            const auto availableBytes =
                static_cast<std::size_t>(data.maxsize - data.chunk->offset);
            const auto validBytes = std::min(
                static_cast<std::size_t>(data.chunk->size), availableBytes);
            const auto  sampleCount = validBytes / BYTES_PER_SAMPLE;
            const auto* bytes =
                static_cast<const std::byte*>(data.data) + data.chunk->offset;
            const auto samples =
                std::span{ reinterpret_cast<const float*>(bytes), sampleCount };
            // 每条边独占自己的写位置；空间不足时 SPSC 丢弃新尾部而不阻塞设备。
            for ( auto* route : runtime.routes ) {
                static_cast<void>(route->ringBuffer.write(samples));
            }
        }
    }
    // PipeWire 继续拥有 pw_buffer，处理后只提交复用，绝不释放其映射内存。
    static_cast<void>(pw_stream_queue_buffer(runtime.stream, buffer));
}

/// @brief 从所有入边读取等长块、应用实时参数并写满播放缓冲。
/// @param userData TargetRuntime 的稳定地址，由 pw_stream 在建流时借用。
/// @warning 由 PipeWire 实时线程调用，所有容器容量和样本区已预分配。
/// @details 目标请求量会夹到 SPA 映射容量与本地最大块；每条边欠载的后缀由
/// SPSC 明确补零，因此混音不会重播上一周期残留。失败的内部契约以整块静音
/// 处理，实时线程不生成日志，平台状态仍保持运行以等待下一块恢复。
void processTarget(void* userData)
{
    // 一个 TargetRuntime 只被一个 playback process 回调消费，无需目标侧锁。
    auto& runtime = *static_cast<TargetRuntime*>(userData);
    auto* buffer  = pw_stream_dequeue_buffer(runtime.stream);
    // PipeWire 可在压力下暂时不给可写缓冲；不能等待或主动分配替代区。
    if ( buffer == nullptr ) return;

    auto* spaBuffer = buffer->buffer;
    // 无 data block 时没有 chunk 可填写，但已出队的外层缓冲仍必须立即归还。
    if ( spaBuffer == nullptr || spaBuffer->n_datas == 0U ) {
        static_cast<void>(pw_stream_queue_buffer(runtime.stream, buffer));
        return;
    }
    auto& data = spaBuffer->datas[0];
    // 映射或 chunk 缺失表示本周期不可写，避免对空地址做 span 转换。
    if ( data.data == nullptr || data.chunk == nullptr ) {
        static_cast<void>(pw_stream_queue_buffer(runtime.stream, buffer));
        return;
    }

    // requested 为播放建议而非强制长度；零表示使用当前缓冲可容纳的完整帧数。
    const auto capacityFrames =
        static_cast<std::size_t>(data.maxsize) / BYTES_PER_FRAME;
    const auto requestedFrames =
        buffer->requested == 0U
            ? capacityFrames
            : std::min(capacityFrames,
                       static_cast<std::size_t>(buffer->requested));
    const auto frameCount  = std::min(requestedFrames, MAX_BLOCK_FRAMES);
    const auto sampleCount = frameCount * ROUTING_CHANNELS;
    // 统一格式保证映射首地址可按 float32 访问，sampleCount 已由 maxsize 夹限。
    auto output = std::span{ static_cast<float*>(data.data), sampleCount };

    // scratch 与 MixInput 均按路由一一对应；循环只推进已有元素和原子读取。
    for ( std::size_t index = 0; index < runtime.routes.size(); ++index ) {
        auto& route        = *runtime.routes[index];
        auto  routeSamples = std::span{ route.scratch }.first(sampleCount);
        static_cast<void>(route.ringBuffer.readOrSilence(routeSamples));
        runtime.mixInputs[index] = Core::MixInput{ .samples = routeSamples,
                                                   .gain    = route.gain(),
                                                   .muted   = route.muted() };
    }
    // Core 先校验所有输入再清空输出；理论契约错误仍以静音保护硬件端点。
    const auto mixed = Core::mixAudio(runtime.mixInputs, output);
    if ( !mixed ) std::ranges::fill(output, 0.0F);

    // 播放端提交从映射起点开始的完整交错帧，stride 明确描述一个双声道 frame。
    data.chunk->offset = 0U;
    data.chunk->size =
        static_cast<std::uint32_t>(sampleCount * BYTES_PER_SAMPLE);
    data.chunk->stride = static_cast<std::int32_t>(BYTES_PER_FRAME);
    static_cast<void>(pw_stream_queue_buffer(runtime.stream, buffer));
}

/// @brief 记录异步建流结果并唤醒等待同步提交的控制线程。
/// @param userData StreamRuntime 基类地址；来源与目标都使用相同开头布局。
/// @param state PipeWire 已提交的新状态。
/// @param error 只在回调期间有效的可空文本，错误态必须复制后再返回。
/// @note 状态事件在 thread-loop 控制线程且已持锁，不属于实时 process 路径。
void streamStateChanged(void* userData, pw_stream_state, pw_stream_state state,
                        const char* error)
{
    auto& runtime = *static_cast<StreamRuntime*>(userData);
    runtime.state = state;
    // 非错误状态不能覆盖此前文本；本轮 StreamRuntime 不会在错误后重新连接。
    if ( state == PW_STREAM_STATE_ERROR ) {
        runtime.error = error == nullptr ? "未知 PipeWire 流错误" : error;
    }
    // false 表示只唤醒 wait，不要求回调等待控制线程 accept，避免控制环互锁。
    pw_thread_loop_signal(runtime.loop, false);
}

/// @brief 捕获流事件表；未使用回调显式留空，避免未来 ABI 字段含未定义值。
const pw_stream_events SOURCE_STREAM_EVENTS{
    .version       = PW_VERSION_STREAM_EVENTS,
    .destroy       = nullptr,
    .state_changed = streamStateChanged,
    .control_info  = nullptr,
    .io_changed    = nullptr,
    .param_changed = nullptr,
    .add_buffer    = nullptr,
    .remove_buffer = nullptr,
    .process       = processSource,
    .drained       = nullptr,
    .command       = nullptr,
    .trigger_done  = nullptr,
};

/// @brief 播放流事件表；状态逻辑共享，但 process 必须指向目标混音入口。
const pw_stream_events TARGET_STREAM_EVENTS{
    .version       = PW_VERSION_STREAM_EVENTS,
    .destroy       = nullptr,
    .state_changed = streamStateChanged,
    .control_info  = nullptr,
    .io_changed    = nullptr,
    .param_changed = nullptr,
    .add_buffer    = nullptr,
    .remove_buffer = nullptr,
    .process       = processTarget,
    .drained       = nullptr,
    .command       = nullptr,
    .trigger_done  = nullptr,
};

/// @brief 使用统一 PCM 格式连接一个定向流，设备适配交给 PipeWire 图完成。
/// @param stream 已设置 target.object 且尚未连接的流。
/// @param direction 捕获使用 INPUT，播放使用 OUTPUT；方向以客户端数据流为准。
/// @return 零为请求已提交，负值为同步 PipeWire 错误码。
/// @details SPA pod 仅在 connect 调用期间借用栈内存；PipeWire 会复制协商参数。
/// AUTOCONNECT 只连到 properties 指定的 serial，DONT_RECONNECT 防止端点移除后
/// 会话管理器把流静默迁移到默认设备。MAP_BUFFERS 使实时回调无需复制映射所有权。
[[nodiscard]] int connectStream(pw_stream* stream, pw_direction direction)
{
    // 1 KiB 足够容纳单个固定 raw-audio EnumFormat pod，且不涉及堆分配。
    std::array<std::byte, 1'024> podStorage{};
    spa_pod_builder              builder{};
    spa_pod_builder_init(&builder,
                         podStorage.data(),
                         static_cast<std::uint32_t>(podStorage.size()));
    // 明确提供声道位置，避免设备适配器按未定义顺序解释双声道样本。
    spa_audio_info_raw format{};
    format.format      = SPA_AUDIO_FORMAT_F32;
    format.rate        = ROUTING_SAMPLE_RATE;
    format.channels    = ROUTING_CHANNELS;
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    format.position[1] = SPA_AUDIO_CHANNEL_FR;
    // connect 的 C API 接收可变指针数组，但每个 pod 本体按 const 契约只读。
    const spa_pod* params[]{ spa_format_audio_raw_build(
        &builder, SPA_PARAM_EnumFormat, &format) };
    // RT_PROCESS 使 process 在数据线程执行，因此上面的回调必须满足实时约束。
    constexpr auto flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_DONT_RECONNECT);
    return pw_stream_connect(stream, direction, PW_ID_ANY, flags, params, 1U);
}

}  // namespace

struct PipeWireRoutingEngine::Implementation {
    /// @brief 析构是最终安全网，保证后端释放前不留下平台回调。
    ~Implementation() { stop(); }

    /// @brief 解析最新图并选择无锁参数更新或完整拓扑切换。
    /// @details 返回成功时，所有在线路由已有对应流，离线路由只保留配置。
    [[nodiscard]] std::expected<void, AudioBackendError> synchronize(
        const Core::RoutingGraph&              graph,
        std::span<const PipeWireSourceBinding> sourceBindings);

    /// @brief 销毁所有流并等待 PipeWire 线程停止后再释放回调状态。
    /// @note 可在空状态和部分启动失败状态幂等调用，不产生控制面错误。
    void stop() noexcept;

    /// @brief 为已解析拓扑预分配工作区、建流并等待异步协商完成。
    /// @param routes 拥有型原生拓扑；成功后移动到 m_topology。
    [[nodiscard]] std::expected<void, AudioBackendError> start(
        std::vector<NativeRoute> routes);

    /// @brief 过滤离线端点并将稳定 Core ID 展开为当前原生节点组合。
    /// @details 物理端点从类别后缀取 serial；应用来源使用枚举缓存，允许一条
    /// 用户路由展开成多个原生采集流。虚拟麦克风在本阶段返回明确错误。
    [[nodiscard]] std::expected<std::vector<NativeRoute>, AudioBackendError>
    resolveRoutes(const Core::RoutingGraph&              graph,
                  std::span<const PipeWireSourceBinding> sourceBindings) const;

    /// @brief 比较仅影响平台流所有权的字段，忽略可原子发布的参数。
    [[nodiscard]] bool topologyMatches(
        std::span<const NativeRoute> routes) const noexcept;

    /// @brief 线程循环在全部 StreamRuntime 后销毁，回调 userData 始终有效。
    pw_thread_loop* m_loop{};

    /// @brief 每个展开后的原生边一个数据通道，地址由 unique_ptr 固定。
    std::vector<std::unique_ptr<RouteRuntime>> m_routes;

    /// @brief 按 sourceObject 聚合，避免同一物理/应用节点为多目标重复捕获。
    std::vector<std::unique_ptr<SourceRuntime>> m_sources;

    /// @brief 按 targetObject 聚合，保证一个播放设备只有一个最终混音回调。
    std::vector<std::unique_ptr<TargetRuntime>> m_targets;

    /// @brief 当前运行流对应的拥有型签名及最近一次参数值。
    std::vector<NativeRoute> m_topology;
};

std::expected<std::vector<NativeRoute>, AudioBackendError>
PipeWireRoutingEngine::Implementation::resolveRoutes(
    const Core::RoutingGraph&              graph,
    std::span<const PipeWireSourceBinding> sourceBindings) const
{
    // 解析发生在 UI 帧外控制线程，允许为拥有型签名分配字符串和 vector。
    std::vector<NativeRoute> resolved;
    for ( const auto& route : graph.routes() ) {
        // 每轮重新按稳定 ID 查询当前端点，绝不缓存 replaceEndpoints 前的指针。
        const auto* source = graph.findSource(route.sourceId);
        const auto* target = graph.findTarget(route.targetId);
        // 离线端点的配置仍留在 Core，但执行层不建立会误连默认设备的流。
        if ( source == nullptr || target == nullptr ) continue;
        if ( target->kind != Core::AudioTargetKind::DeviceOutput ) {
            // 返回错误而非创建普通播放节点冒充系统可见录音设备。
            return std::unexpected(
                routingError("Linux 虚拟麦克风目标尚未实现"));
        }
        const auto targetObject =
            nativeObject(target->id, DEVICE_OUTPUT_PREFIX);
        // 类型前缀不匹配通常意味着跨后端 DTO 被错误传入，应阻止自动连接。
        if ( targetObject.empty() ) {
            return std::unexpected(routingError("无法解析播放设备稳定 ID"));
        }

        if ( source->kind == Core::AudioSourceKind::DeviceInput ) {
            // 设备来源与枚举 object.serial 一一对应，只需展开一条原生执行边。
            const auto sourceObject =
                nativeObject(source->id, DEVICE_INPUT_PREFIX);
            if ( sourceObject.empty() ) {
                return std::unexpected(routingError("无法解析录音设备稳定 ID"));
            }
            // 字符串复制使执行拓扑在下一次端点快照替换后仍独立有效。
            resolved.push_back(
                NativeRoute{ .routeId      = route.id,
                             .sourceObject = std::string{ sourceObject },
                             .targetObject = std::string{ targetObject },
                             .gain         = route.gain,
                             .muted        = route.muted });
            continue;
        }

        // 应用方块按 binary/name 聚合，当前每个 stream 节点都必须独立采集。
        bool foundApplicationNode{};
        for ( const auto& binding : sourceBindings ) {
            if ( binding.sourceId != route.sourceId ) continue;
            foundApplicationNode = true;
            // 多个 NativeRoute 共享 RouteId
            // 与参数，目标混音时自然汇总应用各流。
            resolved.push_back(
                NativeRoute{ .routeId      = route.id,
                             .sourceObject = binding.targetObject,
                             .targetObject = std::string{ targetObject },
                             .gain         = route.gain,
                             .muted        = route.muted });
        }
        if ( !foundApplicationNode ) {
            // 快照中存在应用来源却没有绑定说明枚举状态不完整，不能误连默认源。
            return std::unexpected(
                routingError("应用来源当前没有可连接的 PipeWire 节点"));
        }
    }
    return resolved;
}

bool PipeWireRoutingEngine::Implementation::topologyMatches(
    std::span<const NativeRoute> routes) const noexcept
{
    // 顺序来自 RoutingGraph 创建顺序及 registry 绑定顺序，大小不同必然要重建。
    if ( routes.size() != m_topology.size() ) return false;
    for ( std::size_t index = 0; index < routes.size(); ++index ) {
        // 首个差异即可证明至少一个 stream 分组或 target.object 已失效。
        if ( !routes[index].sameTopology(m_topology[index]) ) return false;
    }
    return true;
}

std::expected<void, AudioBackendError>
PipeWireRoutingEngine::Implementation::synchronize(
    const Core::RoutingGraph&              graph,
    std::span<const PipeWireSourceBinding> sourceBindings)
{
    // 先完整解析再修改运行状态；解析失败时保留上一套仍可工作的流。
    // 这种事务顺序保证新增一条无效边时，其他已经发声的目标不会随之静音。
    // 只有拥有完整的新 NativeRoute 集合后，才决定是否停止当前流对象。
    auto resolved = resolveRoutes(graph, sourceBindings);
    if ( !resolved ) return std::unexpected(std::move(resolved.error()));

    if ( topologyMatches(*resolved) ) {
        // resolved 与运行时缓冲按同一顺序创建；这里只发布标量，不触碰流对象。
        // 应用聚合会让一个 RouteId 对应多个运行时边，逐项更新保证所有当前应用
        // 节点都获得同一用户参数，而实时侧不需要执行字符串或哈希查询。
        for ( std::size_t index = 0; index < resolved->size(); ++index ) {
            m_routes[index]->updateParameters((*resolved)[index].gain,
                                              (*resolved)[index].muted);
            m_topology[index].gain  = (*resolved)[index].gain;
            m_topology[index].muted = (*resolved)[index].muted;
        }
        // release 发布后，目标周期 acquire 会分别取得两个独立参数的最新值。
        return {};
    }

    // 拓扑切换优先停止旧回调，随后释放所有旧指针，再构造新一代状态。
    // 新建失败时保持明确空执行状态，不能继续使用与当前图不一致的旧线路。
    // UI 会保留错误供用户刷新或删除失败边后重新同步。
    stop();
    return start(std::move(*resolved));
}

std::expected<void, AudioBackendError>
PipeWireRoutingEngine::Implementation::start(std::vector<NativeRoute> routes)
{
    // 空拓扑表示最后一条在线路由已删除；无需保留空线程或 PipeWire client。
    if ( routes.empty() ) {
        m_topology.clear();
        return {};
    }

    // 先完成所有拥有型工作区和回调索引分组，再允许 PipeWire 启动实时处理。
    // unique_ptr 固定每个 RouteRuntime 地址，即使外层 vector
    // 扩容也不影响回调表。
    // 工作区按展开后的原生边分配，因为多个应用节点不能共同充当一个 SPSC 的
    // 生产者；拆分后目标回调仍会把它们作为同一用户路由统一求和。
    m_routes.reserve(routes.size());
    for ( const auto& route : routes ) {
        m_routes.push_back(std::make_unique<RouteRuntime>(route));
    }
    // 每条原生边同时进入来源扇出组和目标汇入组；两个组都只借用 route。
    for ( std::size_t index = 0; index < routes.size(); ++index ) {
        const auto& route = routes[index];
        auto        source =
            std::ranges::find_if(m_sources, [&](const auto& candidate) {
                return candidate->targetObject == route.sourceObject;
            });
        if ( source == m_sources.end() ) {
            // 首次遇到原生来源时建立唯一采集状态，后续相同来源只追加扇出边。
            auto runtime          = std::make_unique<SourceRuntime>();
            runtime->targetObject = route.sourceObject;
            runtime->routes.push_back(m_routes[index].get());
            m_sources.push_back(std::move(runtime));
        } else {
            // RouteRuntime 仍独立，确保不同目标拥有各自的 SPSC 消费位置。
            (*source)->routes.push_back(m_routes[index].get());
        }

        auto target =
            std::ranges::find_if(m_targets, [&](const auto& candidate) {
                return candidate->targetObject == route.targetObject;
            });
        if ( target == m_targets.end() ) {
            // 首次遇到目标时建立唯一播放状态，所有入边在该回调统一限幅。
            auto runtime          = std::make_unique<TargetRuntime>();
            runtime->targetObject = route.targetObject;
            runtime->routes.push_back(m_routes[index].get());
            m_targets.push_back(std::move(runtime));
        } else {
            // 追加只在线程启动前发生，实时回调不会观察 vector 扩容或中间状态。
            (*target)->routes.push_back(m_routes[index].get());
        }
    }
    for ( auto& target : m_targets ) {
        // MixInput 元素数与 routes 固定一致，processTarget 只覆盖已有元素。
        target->mixInputs.resize(target->routes.size());
    }

    // 一个 thread-loop 管理本轮全部流的控制事件；process 可由 PipeWire 数据
    // 线程并发触发，但每个 SPSC 的生产者/消费者角色已由上面的分组唯一确定。
    // simple stream 各自管理 core/remote；枚举使用的短生命周期 main-loop 与这里
    // 完全分离，刷新结束后没有任何 registry 代理被实时执行层借用。
    m_loop = pw_thread_loop_new("AudioRoads routing", nullptr);
    if ( m_loop == nullptr ) {
        stop();
        return std::unexpected(routingError("无法创建实时线程循环"));
    }
    const auto startResult = pw_thread_loop_start(m_loop);
    if ( startResult < 0 ) {
        // 线程未启动时 stop 仍可销毁 loop 和尚未暴露给回调的工作区。
        stop();
        return std::unexpected(routingError("无法启动实时线程循环，错误码 " +
                                            std::to_string(startResult)));
    }

    // PipeWire 对象只能在持有 thread-loop 锁时从外部线程访问；任一步失败先
    // 记录文本，统一解锁后再走 stop，避免各早退分支遗漏部分创建的流。
    std::string creationError;
    pw_thread_loop_lock(m_loop);
    for ( auto& source : m_sources ) {
        source->loop = m_loop;
        // target.object 使用枚举得到的 serial，AUTOCONNECT 不会选择默认输入。
        // Communication 只描述流用途，不改变 AudioRoads 路由语义。
        auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE,
                                             "Audio",
                                             PW_KEY_MEDIA_CATEGORY,
                                             "Capture",
                                             PW_KEY_MEDIA_ROLE,
                                             "Communication",
                                             PW_KEY_TARGET_OBJECT,
                                             source->targetObject.c_str(),
                                             nullptr);
        source->stream   = pw_stream_new_simple(pw_thread_loop_get_loop(m_loop),
                                                "AudioRoads capture",
                                                properties,
                                                &SOURCE_STREAM_EVENTS,
                                                source.get());
        if ( source->stream == nullptr ) {
            // 创建失败时保留此前已建流，交由统一 stop 按正确锁顺序回收。
            creationError = "无法创建来源采集流";
            break;
        }
        const auto result = connectStream(source->stream, PW_DIRECTION_INPUT);
        if ( result < 0 ) {
            // INPUT 表示客户端消费设备/应用节点输出，与 Core source 方向一致。
            creationError =
                "无法连接来源采集流，错误码 " + std::to_string(result);
            break;
        }
    }
    if ( creationError.empty() ) {
        // 所有来源请求提交后再建目标，缩短播放回调仅收到静音的启动窗口。
        for ( auto& target : m_targets ) {
            target->loop = m_loop;
            // 每个目标组只产生一个 Playback stream，再由 PipeWire 定向物理
            // sink。
            auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE,
                                                 "Audio",
                                                 PW_KEY_MEDIA_CATEGORY,
                                                 "Playback",
                                                 PW_KEY_MEDIA_ROLE,
                                                 "Communication",
                                                 PW_KEY_TARGET_OBJECT,
                                                 target->targetObject.c_str(),
                                                 nullptr);
            target->stream =
                pw_stream_new_simple(pw_thread_loop_get_loop(m_loop),
                                     "AudioRoads playback",
                                     properties,
                                     &TARGET_STREAM_EVENTS,
                                     target.get());
            if ( target->stream == nullptr ) {
                // 已建立的来源继续由统一失败清理销毁，不在循环中交错改容器。
                creationError = "无法创建目标播放流";
                break;
            }
            const auto result =
                connectStream(target->stream, PW_DIRECTION_OUTPUT);
            if ( result < 0 ) {
                // OUTPUT 表示客户端产出混音数据，设备 sink 在链接另一端消费它。
                creationError =
                    "无法连接目标播放流，错误码 " + std::to_string(result);
                break;
            }
        }
    }
    if ( creationError.empty() ) {
        // connect 只提交异步请求；等待 PAUSED/STREAMING 才能向 UI
        // 声称路由已建立。
        // 不等待会造成假成功：target.object 拼错时 connect
        // 可能先返回零，随后状态 回调才进入
        // ERROR，而画布已经显示出看似工作的连接。
        // 使用单个绝对期限，多个状态事件唤醒不会重复延长用户可见操作时间。
        timespec               deadline{};
        constexpr std::int64_t CONNECT_TIMEOUT_NS = 2'000'000'000;
        if ( pw_thread_loop_get_time(m_loop, &deadline, CONNECT_TIMEOUT_NS) <
             0 ) {
            creationError = "无法建立 PipeWire 建流超时边界";
        }
        while ( creationError.empty() ) {
            // 状态回调和本循环都持有 thread-loop 锁；wait
            // 会暂时释放锁等待事件。
            bool       allReady = true;
            const auto inspect  = [&](const StreamRuntime& runtime) {
                if ( runtime.state == PW_STREAM_STATE_ERROR ) {
                    // 文本已由控制回调复制，退出锁域后可安全移动到 UI。
                    creationError = runtime.error.empty()
                                        ? "PipeWire 流进入错误状态"
                                        : runtime.error;
                    return;
                }
                if ( runtime.state != PW_STREAM_STATE_PAUSED &&
                     runtime.state != PW_STREAM_STATE_STREAMING ) {
                    // CONNECTING/UNCONNECTED 都不能证明会话管理器已接受格式。
                    allReady = false;
                }
            };
            for ( const auto& source : m_sources ) inspect(*source);
            for ( const auto& target : m_targets ) inspect(*target);
            // PAUSED 已完成格式和缓冲协商；图开始运行后会自动进入 STREAMING。
            if ( !creationError.empty() || allReady ) break;
            if ( pw_thread_loop_timed_wait_full(m_loop, &deadline) != 0 ) {
                // 超时不保留半连接流，避免 UI 蓝线再次与实际执行状态不一致。
                creationError = "等待 PipeWire 流就绪超时";
            }
        }
    }
    pw_thread_loop_unlock(m_loop);

    if ( !creationError.empty() ) {
        // stop 会重新持锁销毁成功创建的前缀，再停止线程并释放所有工作区。
        stop();
        return std::unexpected(routingError(std::move(creationError)));
    }
    // 只有全部流就绪后才提交签名，参数更新不会误命中失败的旧拓扑。
    m_topology = std::move(routes);
    return {};
}

void PipeWireRoutingEngine::Implementation::stop() noexcept
{
    // 正常删线、拓扑重建、部分 start 失败和后端析构都走同一幂等清理路径。
    // 统一顺序避免错误分支只释放一部分流，留下仍引用 RouteRuntime 的回调。
    if ( m_loop != nullptr ) {
        // Stream 销毁必须持有控制循环锁；destroy 返回后不再产生对应 userData
        // 回调。
        pw_thread_loop_lock(m_loop);
        // 先停消费者，避免来源仍在销毁过程中向已经失去目标的缓冲持续写入。
        for ( auto& target : m_targets ) {
            if ( target->stream != nullptr ) {
                // destroy 断开图链接，并同步结束该 stream 对 userData 的访问。
                pw_stream_destroy(target->stream);
                target->stream = nullptr;
            }
        }
        // 所有目标回调结束后再释放采集流，RouteRuntime 此刻仍全部存活。
        for ( auto& source : m_sources ) {
            if ( source->stream != nullptr ) {
                pw_stream_destroy(source->stream);
                source->stream = nullptr;
            }
        }
        pw_thread_loop_unlock(m_loop);
        // stop 按 PipeWire 契约不能持有 loop 锁；返回后控制回调也已停止。
        pw_thread_loop_stop(m_loop);
        // thread-loop 最后销毁，所有附属 simple stream 已在上面显式释放。
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
    }
    // 非拥有指针容器先于 RouteRuntime 清空，维持明确的逻辑生命周期顺序。
    m_targets.clear();
    m_sources.clear();
    m_routes.clear();
    // 签名与流同生共灭，空状态保证下一次同步一定重建非空拓扑。
    m_topology.clear();
}

/// @brief 创建不含平台句柄的空执行器；PipeWire 资源按首条路由延迟建立。
PipeWireRoutingEngine::PipeWireRoutingEngine()
    : m_implementation(std::make_unique<Implementation>())
{
    // PImpl 只在控制面分配一次；真正 PCM 工作区在首次路由同步时按拓扑预分配。
}

/// @brief 通过 Implementation 析构先停回调，再释放所有预分配工作区。
PipeWireRoutingEngine::~PipeWireRoutingEngine() = default;

/// @brief 公共平台边界只转发拥有型实现，不泄漏 PImpl 或 PipeWire 头。
std::expected<void, AudioBackendError> PipeWireRoutingEngine::synchronize(
    const Core::RoutingGraph&              graph,
    std::span<const PipeWireSourceBinding> sourceBindings)
{
    return m_implementation->synchronize(graph, sourceBindings);
}

}  // namespace AudioRoads::Audio
