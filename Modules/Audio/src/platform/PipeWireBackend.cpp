#include "IAudioBackend.h"

#include <pipewire/keys.h>
#include <pipewire/pipewire.h>

#include <spa/utils/dict.h>

#include <charconv>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AudioRoads::Audio
{
namespace
{

/// @brief 单次 PipeWire registry round-trip 使用的临时状态。
///
/// 该对象由 enumerateDevices 的栈帧拥有，地址作为 userData 借给 listener。
/// 只有主循环退出且 listener 全部移除后才能销毁，因此回调无需额外同步。
/// 它只收集控制面快照，不得被音频处理线程观察或持有。
struct EnumerationState {
    /// @brief 驱动同步发现流程的主循环。
    pw_main_loop* loop{};

    /// @brief 用于匹配 core.done 回调的同步序号。
    int syncSequence{};

    /// @brief 完整 registry 快照转换出的音频设备。
    std::vector<Core::AudioDevice> devices;

    /// @brief 服务端异步错误；非空时枚举失败。
    std::string error;
};

/// @brief 读取字典字符串，并为缺失属性提供空视图。
///
/// 返回的 string_view 借用 spa_dict 内存，只允许在当前 registry 回调内读取。
/// 需要进入 Core DTO 的字段必须在回调返回前复制成拥有型字符串。
/// @return 属性缺失、字典为空时均返回空视图，调用方负责选择降级语义。
[[nodiscard]] std::string_view property(const spa_dict* properties,
                                        const char*     key) noexcept
{
    // 属性缺失在 PipeWire 节点中是正常情况，统一为空视图可让调用点显式降级。
    if ( properties == nullptr ) return {};
    const auto* value = spa_dict_lookup(properties, key);
    // spa_dict 保持字符串所有权，返回视图不触发控制面分配。
    return value == nullptr ? std::string_view{} : std::string_view{ value };
}

/// @brief 将 PipeWire 字符串属性转换为无符号数，非法值按未知处理。
///
/// PipeWire 的通道数和采样率属性并非每个节点都存在。零值在 AudioDevice 中
/// 表示未知，避免因单个展示属性格式异常而丢弃整个可用端点。
[[nodiscard]] std::uint32_t parseUnsigned(std::string_view text) noexcept
{
    std::uint32_t value{};
    // from_chars 不使用 locale 且不抛异常，适合平台属性到标量 DTO 的边界。
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : 0U;
}

/// @brief 接收 registry global 并筛选 Audio/Source、Audio/Sink 节点。
///
/// registry 还包含客户端流、过滤器和 monitor 等内部节点，它们不能作为用户
/// 可打开的端点。回调只复制稳定标识、展示名和基础能力，不保留任何原生代理。
/// @param userData 指向本次枚举栈上的 EnumerationState，listener 移除前有效。
void onRegistryGlobal(void* userData, std::uint32_t id, std::uint32_t,
                      const char*     type, std::uint32_t,
                      const spa_dict* properties)
{
    auto& state = *static_cast<EnumerationState*>(userData);
    // registry 同时广播多种接口，先按类型过滤再读取 Node 专属属性。
    if ( type == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 ) {
        return;
    }

    const auto       mediaClass = property(properties, PW_KEY_MEDIA_CLASS);
    Core::DeviceFlow flow{};
    if ( mediaClass == "Audio/Source" ) {
        flow = Core::DeviceFlow::Input;
    } else if ( mediaClass == "Audio/Sink" ) {
        flow = Core::DeviceFlow::Output;
    } else {
        // Stream、filter 和 monitor 等节点不是用户可直接打开的硬件端点。
        return;
    }

    auto stableId = property(properties, PW_KEY_OBJECT_SERIAL);
    // serial 在节点重命名后仍较稳定；缺失时才退回 node.name，绝不把代理指针
    // 之类的进程内地址泄漏进可持久化的 Core DTO。
    if ( stableId.empty() ) stableId = property(properties, PW_KEY_NODE_NAME);
    const auto description = property(properties, PW_KEY_NODE_DESCRIPTION);
    const auto nick        = property(properties, PW_KEY_NODE_NICK);
    const auto nodeName    = property(properties, PW_KEY_NODE_NAME);

    std::string displayName;
    // 可读名称逐级降级；最后使用本次 registry ID，保证 UI 永远有非空标签。
    if ( !description.empty() ) {
        displayName.assign(description);
    } else if ( !nick.empty() ) {
        displayName.assign(nick);
    } else if ( !nodeName.empty() ) {
        displayName.assign(nodeName);
    } else {
        displayName = "PipeWire node " + std::to_string(id);
    }

    // PipeWire 属性是借用的 spa_dict 字符串，离开回调前必须全部复制或解析。
    const auto channels = parseUnsigned(property(properties, "audio.channels"));
    const auto rate     = parseUnsigned(property(properties, "audio.rate"));
    const auto isInput  = flow == Core::DeviceFlow::Input;
    // Source/Sink 分别只填写一个方向的通道数，零表示另一方向不具备能力。
    state.devices.push_back(Core::AudioDevice{
        .id = "pipewire:" +
              (stableId.empty() ? std::to_string(id) : std::string{ stableId }),
        .name           = std::move(displayName),
        .backend        = "PipeWire",
        .flow           = flow,
        .inputChannels  = isInput ? channels : 0U,
        .outputChannels = isInput ? 0U : channels,
        .sampleRate     = rate,
        // 默认端点需要 PipeWire metadata；尚未接入时明确保持 false，不猜测。
        .isDefault = false,
    });
}

/// @brief 完成显式 sync 后退出主循环，保证此前 global 已全部送达。
///
/// PipeWire 可能同时发送其他 done，只有 core 自身且序号匹配的事件才是本次
/// 快照边界；过早退出会产生随机缺失设备的部分快照。
void onCoreDone(void* userData, std::uint32_t id, int sequence)
{
    auto& state = *static_cast<EnumerationState*>(userData);
    if ( id == PW_ID_CORE && sequence == state.syncSequence ) {
        // 退出只结束本次同步等待，实际对象销毁由 enumerateDevices
        // 统一逆序执行。
        pw_main_loop_quit(state.loop);
    }
}

/// @brief 捕获连接级错误并停止等待，避免控制面永久阻塞。
///
/// 错误回调可能早于预期 done 到达，因此先复制服务端文本，再主动结束主循环。
/// enumerateDevices 在资源清理完成后把该文本转换为 AudioBackendError。
void onCoreError(void* userData, std::uint32_t, int, int result,
                 const char* message)
{
    auto& state = *static_cast<EnumerationState*>(userData);
    // 先记录数值结果，服务端 message 可选且只在回调期间有效，存在时必须复制。
    state.error = "PipeWire error " + std::to_string(result);
    if ( message != nullptr ) state.error += ": " + std::string{ message };
    pw_main_loop_quit(state.loop);
}

/// @brief 负责 PipeWire 全局初始化，并按需执行同步设备发现。
/// @warning 枚举会阻塞等待 registry round-trip，只能从低频控制面调用。
///
/// pw_init/pw_deinit 由对象寿命配平；单次枚举使用的 loop、context、core、
/// registry 与 listener 均保持局部，不跨刷新缓存已失效的服务端对象。
class PipeWireBackend final : public IAudioBackend
{
public:
    /// @brief 初始化当前进程的 PipeWire 客户端支持。
    PipeWireBackend() { pw_init(nullptr, nullptr); }

    /// @brief 平衡构造阶段的全局初始化。
    ~PipeWireBackend() override
    {
        // 全部单次枚举对象已在调用结束前销毁，最后才能平衡进程级初始化。
        pw_deinit();
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "PipeWire";
    }

    [[nodiscard]] std::expected<std::vector<Core::AudioDevice>,
                                AudioBackendError>
    enumerateDevices() override;
};

std::expected<std::vector<Core::AudioDevice>, AudioBackendError>
PipeWireBackend::enumerateDevices()
{
    // 每次刷新使用独立对象树，调用结束后不缓存任何 pw_proxy 或 spa 指针。
    // 早退分支按已成功创建资源的逆序释放；后续若增加资源需保持同一不变量。
    auto* loop = pw_main_loop_new(nullptr);
    // loop 是对象树根，创建失败时尚无任何 PipeWire 局部资源需要清理。
    if ( loop == nullptr ) {
        return std::unexpected(AudioBackendError{
            .operation = "创建 PipeWire 主循环", .message = "返回空指针" });
    }

    auto* context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
    if ( context == nullptr ) {
        // context 尚未建立时只需回收 loop，不能调用依赖 context 的清理函数。
        pw_main_loop_destroy(loop);
        return std::unexpected(AudioBackendError{
            .operation = "创建 PipeWire context", .message = "返回空指针" });
    }

    auto* core = pw_context_connect(context, nullptr, 0);
    // 连接使用当前用户默认远端，尊重 PipeWire 环境与会话服务配置。
    if ( core == nullptr ) {
        // 连接失败仍需先销毁 context，再销毁它引用的 loop。
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
        return std::unexpected(AudioBackendError{
            .operation = "连接 PipeWire", .message = "无法连接用户音频服务" });
    }

    auto* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    // registry 代理是设备发现的唯一事件源，本次实现不订阅 metadata 或流节点。
    if ( registry == nullptr ) {
        // registry 未创建，不可调用 proxy destroy；只逆序断开已拥有的对象。
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
        return std::unexpected(AudioBackendError{
            .operation = "取得 PipeWire registry", .message = "返回空指针" });
    }

    // state 必须声明在 hooks 之前并活到清理结束，确保所有 listener 的 userData
    // 在可能发生回调的整个期间保持有效。
    EnumerationState state{
        .loop = loop, .syncSequence = 0, .devices = {}, .error = {}
    };
    spa_hook           registryListener{};
    spa_hook           coreListener{};
    pw_registry_events registryEvents{};
    registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
    registryEvents.global  = onRegistryGlobal;
    // 事件结构零初始化，仅填入当前版本支持且本次快照需要的回调。
    pw_core_events coreEvents{};
    // core listener 同时观察成功边界和连接错误，保证等待一定有可处理终点。
    coreEvents.version = PW_VERSION_CORE_EVENTS;
    coreEvents.done    = onCoreDone;
    coreEvents.error   = onCoreError;

    pw_registry_add_listener(
        registry, &registryListener, &registryEvents, &state);
    pw_core_add_listener(core, &coreListener, &coreEvents, &state);
    // sync 序号是快照边界：匹配的 done 到达时，此前的 global 事件均已分发。
    state.syncSequence = pw_core_sync(core, PW_ID_CORE, 0);
    if ( state.syncSequence < 0 ) {
        // sync 提交失败时主循环不会产生匹配 done，直接进入统一清理避免死等。
        state.error = "无法提交 PipeWire registry 同步";
    } else {
        // run 阻塞至匹配 done 或 error 回调退出；禁止从实时线程和 ImGui
        // 绘制栈调用。
        pw_main_loop_run(loop);
    }

    // 监听器必须先于其代理和 context 移除，避免销毁阶段回调悬空状态。
    spa_hook_remove(&coreListener);
    spa_hook_remove(&registryListener);
    // listener 移除后 userData 不再被访问，随后才可销毁代理及其父对象。
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    if ( !state.error.empty() ) {
        // 只有完成全部原生资源清理后才移动错误，确保返回路径没有悬空回调。
        return std::unexpected(
            AudioBackendError{ .operation = "枚举 PipeWire 设备",
                               .message   = std::move(state.error) });
    }
    // 每个 DTO 都拥有自身字符串；移动容器后不依赖已销毁的 registry 状态。
    return std::move(state.devices);
}

}  // namespace

std::unique_ptr<IAudioBackend> createPipeWireBackend()
{
    // 构造时平衡 PipeWire 全局初始化，返回后由 AudioService 独占其寿命。
    return std::make_unique<PipeWireBackend>();
}

}  // namespace AudioRoads::Audio
