#include "IAudioBackend.h"

#include <pipewire/keys.h>
#include <pipewire/pipewire.h>

#include <spa/utils/dict.h>

#include <algorithm>
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
/// 该对象由 enumerateEndpoints 的栈帧拥有，地址作为 userData 借给 listener。
/// 只有主循环退出且 listener 全部移除后才能销毁，因此回调无需额外同步。
/// 它只收集控制面快照，不得被音频处理线程观察或持有。
struct EnumerationState {
    /// @brief 驱动同步发现流程的主循环。
    pw_main_loop* loop{};

    /// @brief 用于匹配 core.done 回调的同步序号。
    int syncSequence{};

    /// @brief 完整 registry 事件转换出的来源与目标快照。
    Core::AudioEndpointSnapshot endpoints;

    /// @brief 服务端异步错误；非空时枚举失败。
    /// @note 由同一 pw_main_loop 线程写入，枚举返回后才由控制线程读取。
    std::string error;
};

/// @brief 读取字典字符串，并为缺失属性提供空视图。
///
/// 返回的 string_view 借用 spa_dict 内存，只允许在当前 registry 回调内读取。
/// 需要进入 Core DTO 的字段必须在回调返回前复制成拥有型字符串。
/// @return 属性缺失、字典为空时均返回空视图，调用方负责选择降级语义。
/// @warning 返回值不能保存到 DTO、异步任务或下一次 PipeWire 回调。
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
/// PipeWire 的通道数和采样率属性并非每个节点都存在。零值在端点 DTO 中
/// 表示未知，避免因单个展示属性格式异常而丢弃整个可用端点。
/// @param text 当前 registry 回调内借用的十进制属性。
/// @return 完整解析的非负值；缺失、溢出或含尾随字符时为零。
[[nodiscard]] std::uint32_t parseUnsigned(std::string_view text) noexcept
{
    // 缺失属性返回空视图；先短路可避免把空指针交给 from_chars 的范围契约。
    if ( text.empty() ) return 0U;
    std::uint32_t value{};
    // from_chars 不使用 locale 且不抛异常，适合平台属性到标量 DTO 的边界。
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    // 必须同时消费完整字符串，避免把 "48000-invalid" 当成合法采样率。
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
               ? value
               : 0U;
}

/// @brief 接收 registry global 并分类设备端点与桌面应用输出流。
///
/// Audio/Source 和 Audio/Sink 分别成为设备来源与播放目标；
/// Stream/Output/Audio 按应用身份聚合为应用输出来源。其他过滤器和 monitor
/// 等内部节点不暴露给用户。回调不保留任何原生代理。
/// @param userData 指向本次枚举栈上的 EnumerationState，listener 移除前有效。
/// @param id 当前远端连接内的临时 global ID，只在缺少稳定属性时降级使用。
/// @param type global 接口类型；只有 Node 进入端点分类。
/// @param properties 此次回调借用的 SPA 字典，返回后全部视图失效。
void onRegistryGlobal(void* userData, std::uint32_t id, std::uint32_t,
                      const char*     type, std::uint32_t,
                      const spa_dict* properties)
{
    auto& state = *static_cast<EnumerationState*>(userData);
    // registry 同时广播多种接口，先按类型过滤再读取 Node 专属属性。
    if ( type == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 ) {
        return;
    }

    const auto mediaClass          = property(properties, PW_KEY_MEDIA_CLASS);
    const auto isDeviceInput       = mediaClass == "Audio/Source";
    const auto isDeviceOutput      = mediaClass == "Audio/Sink";
    const auto isApplicationOutput = mediaClass == "Stream/Output/Audio";
    if ( !isDeviceInput && !isDeviceOutput && !isApplicationOutput ) {
        // Filter、monitor 和其他内部节点不是本阶段可直接路由的用户端点。
        return;
    }

    auto stableId = property(properties, PW_KEY_OBJECT_SERIAL);
    // serial 在节点重命名后仍较稳定；缺失时才退回 node.name，绝不把代理指针
    // 之类的进程内地址泄漏进可持久化的 Core DTO。
    if ( stableId.empty() ) stableId = property(properties, PW_KEY_NODE_NAME);
    const auto description = property(properties, PW_KEY_NODE_DESCRIPTION);
    const auto nick        = property(properties, PW_KEY_NODE_NICK);
    const auto nodeName    = property(properties, PW_KEY_NODE_NAME);
    const auto appName     = property(properties, PW_KEY_APP_NAME);
    const auto appBinary   = property(properties, PW_KEY_APP_PROCESS_BINARY);
    const auto processId =
        parseUnsigned(property(properties, PW_KEY_APP_PROCESS_ID));

    std::string displayName;
    // 可读名称逐级降级；最后使用本次 registry ID，保证 UI 永远有非空标签。
    if ( isApplicationOutput && !appName.empty() ) {
        displayName.assign(appName);
    } else if ( !description.empty() ) {
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
    const auto nativeId =
        stableId.empty() ? std::to_string(id) : std::string{ stableId };

    if ( isDeviceInput ) {
        // PipeWire 的 Audio/Source 从图节点向客户端提供数据，对路由引擎是
        // source。
        state.endpoints.sources.push_back(Core::AudioSource{
            .id         = "pipewire:device-input:" + nativeId,
            .name       = std::move(displayName),
            .backend    = "PipeWire",
            .kind       = Core::AudioSourceKind::DeviceInput,
            .channels   = channels,
            .sampleRate = rate,
            // 默认端点需要 metadata；尚未接入时明确保持 false，不猜测。
            .isDefault   = false,
            .application = std::nullopt,
        });
        return;
    }

    if ( isDeviceOutput ) {
        // Audio/Sink 接收客户端播放数据，对路由引擎是
        // target；名称相似也不合并。
        state.endpoints.targets.push_back(Core::AudioTarget{
            .id         = "pipewire:device-output:" + nativeId,
            .name       = std::move(displayName),
            .backend    = "PipeWire",
            .kind       = Core::AudioTargetKind::DeviceOutput,
            .channels   = channels,
            .sampleRate = rate,
            .isDefault  = false,
        });
        return;
    }

    // 同一应用可创建多条 PipeWire stream。路由选择按 binary/name 聚合，未来
    // 建流时由后端把当前匹配节点共同接入，而不是让用户逐条选择短生命周期流。
    const auto applicationKey = !appBinary.empty() ? std::string{ appBinary }
                                : !appName.empty() ? std::string{ appName }
                                                   : nativeId;
    // 类别前缀把应用流与同名物理设备隔离，也给后续开流分派提供无歧义入口。
    const auto sourceId  = "pipewire:application:" + applicationKey;
    const auto duplicate = std::ranges::find(
        state.endpoints.sources, sourceId, &Core::AudioSource::id);
    if ( duplicate != state.endpoints.sources.end() ) {
        // 多进程应用及同一进程的多条 stream 共用一个 UI 来源；PID 仅追加一次。
        auto& processIds = duplicate->application->processIds;
        if ( processId != 0 &&
             std::ranges::find(processIds, processId) == processIds.end() ) {
            processIds.push_back(processId);
        }
        return;
    }

    state.endpoints.sources.push_back(Core::AudioSource{
        .id         = sourceId,
        .name       = std::move(displayName),
        .backend    = "PipeWire",
        .kind       = Core::AudioSourceKind::ApplicationOutput,
        .channels   = channels,
        .sampleRate = rate,
        .isDefault  = false,
        .application =
            Core::ApplicationIdentity{
                .processIds = processId == 0
                                  ? std::vector<std::uint64_t>{}
                                  : std::vector<std::uint64_t>{ processId },
                .stableId   = applicationKey },
    });
}

/// @brief 完成显式 sync 后退出主循环，保证此前 global 已全部送达。
///
/// PipeWire 可能同时发送其他 done，只有 core 自身且序号匹配的事件才是本次
/// 快照边界；过早退出会产生随机缺失设备的部分快照。
/// @param userData 当前同步轮次的 EnumerationState。
/// @param id 发出 done 的代理 ID，必须是 PW_ID_CORE。
/// @param sequence 服务端回送的序号，必须匹配本轮 pw_core_sync。
void onCoreDone(void* userData, std::uint32_t id, int sequence)
{
    auto& state = *static_cast<EnumerationState*>(userData);
    if ( id == PW_ID_CORE && sequence == state.syncSequence ) {
        // 退出只结束本次同步等待，实际对象销毁由 enumerateEndpoints
        // 统一逆序执行。
        pw_main_loop_quit(state.loop);
    }
}

/// @brief 捕获连接级错误并停止等待，避免控制面永久阻塞。
///
/// 错误回调可能早于预期 done 到达，因此先复制服务端文本，再主动结束主循环。
/// enumerateEndpoints 在资源清理完成后把该文本转换为 AudioBackendError。
/// @param result PipeWire 负 errno 风格结果，原值进入诊断文本。
/// @param message 只在当前回调内有效的可空服务端字符串。
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

    /// @brief 完成一次 registry 同步边界内的来源与目标发现。
    [[nodiscard]] std::expected<Core::AudioEndpointSnapshot, AudioBackendError>
    enumerateEndpoints() override;
};

std::expected<Core::AudioEndpointSnapshot, AudioBackendError>
PipeWireBackend::enumerateEndpoints()
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
    // context 借用 loop 的底层 pw_loop，销毁顺序必须始终先 context 后 loop。
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
        .loop = loop, .syncSequence = 0, .endpoints = {}, .error = {}
    };
    spa_hook registryListener{};
    spa_hook coreListener{};
    // events 结构必须零初始化，未使用函数指针保持空值而不是未定义字节。
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
    // listener 完成注册后再发 sync，保证快照边界覆盖所有初始 global 事件。
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
    // registry 是从 core 取得的 proxy，父 core 断开之前显式释放其代理引用。
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
    // 此时 listener 已移除且所有原生对象已销毁，返回值只包含 Core 值类型。
    return std::move(state.endpoints);
}

}  // namespace

std::unique_ptr<IAudioBackend> createPipeWireBackend()
{
    // 构造时平衡 PipeWire 全局初始化，返回后由 AudioService 独占其寿命。
    return std::make_unique<PipeWireBackend>();
}

}  // namespace AudioRoads::Audio
