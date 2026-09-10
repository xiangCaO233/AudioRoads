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
[[nodiscard]] std::string_view property(const spa_dict* properties,
                                        const char*     key) noexcept
{
    if ( properties == nullptr ) return {};
    const auto* value = spa_dict_lookup(properties, key);
    return value == nullptr ? std::string_view{} : std::string_view{ value };
}

/// @brief 将 PipeWire 字符串属性转换为无符号数，非法值按未知处理。
[[nodiscard]] std::uint32_t parseUnsigned(std::string_view text) noexcept
{
    std::uint32_t value{};
    const auto    result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : 0U;
}

/// @brief 接收 registry global 并筛选 Audio/Source、Audio/Sink 节点。
void onRegistryGlobal(void* userData, std::uint32_t id, std::uint32_t,
                      const char*     type, std::uint32_t,
                      const spa_dict* properties)
{
    auto& state = *static_cast<EnumerationState*>(userData);
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
    if ( stableId.empty() ) stableId = property(properties, PW_KEY_NODE_NAME);
    const auto description = property(properties, PW_KEY_NODE_DESCRIPTION);
    const auto nick        = property(properties, PW_KEY_NODE_NICK);
    const auto nodeName    = property(properties, PW_KEY_NODE_NAME);

    std::string displayName;
    if ( !description.empty() ) {
        displayName.assign(description);
    } else if ( !nick.empty() ) {
        displayName.assign(nick);
    } else if ( !nodeName.empty() ) {
        displayName.assign(nodeName);
    } else {
        displayName = "PipeWire node " + std::to_string(id);
    }

    const auto channels = parseUnsigned(property(properties, "audio.channels"));
    const auto rate     = parseUnsigned(property(properties, "audio.rate"));
    const auto isInput  = flow == Core::DeviceFlow::Input;
    state.devices.push_back(Core::AudioDevice{
        .id = "pipewire:" +
              (stableId.empty() ? std::to_string(id) : std::string{ stableId }),
        .name           = std::move(displayName),
        .backend        = "PipeWire",
        .flow           = flow,
        .inputChannels  = isInput ? channels : 0U,
        .outputChannels = isInput ? 0U : channels,
        .sampleRate     = rate,
        .isDefault      = false,
    });
}

/// @brief 完成显式 sync 后退出主循环，保证此前 global 已全部送达。
void onCoreDone(void* userData, std::uint32_t id, int sequence)
{
    auto& state = *static_cast<EnumerationState*>(userData);
    if ( id == PW_ID_CORE && sequence == state.syncSequence ) {
        pw_main_loop_quit(state.loop);
    }
}

/// @brief 捕获连接级错误并停止等待，避免 UI 永久阻塞。
void onCoreError(void* userData, std::uint32_t, int, int result,
                 const char* message)
{
    auto& state = *static_cast<EnumerationState*>(userData);
    state.error = "PipeWire error " + std::to_string(result);
    if ( message != nullptr ) state.error += ": " + std::string{ message };
    pw_main_loop_quit(state.loop);
}

/// @brief 负责 PipeWire 全局初始化，并按需执行同步设备发现。
class PipeWireBackend final : public IAudioBackend
{
public:
    /// @brief 初始化当前进程的 PipeWire 客户端支持。
    PipeWireBackend() { pw_init(nullptr, nullptr); }

    /// @brief 平衡构造阶段的全局初始化。
    ~PipeWireBackend() override { pw_deinit(); }

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
    auto* loop = pw_main_loop_new(nullptr);
    if ( loop == nullptr ) {
        return std::unexpected(AudioBackendError{
            .operation = "创建 PipeWire 主循环", .message = "返回空指针" });
    }

    auto* context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
    if ( context == nullptr ) {
        pw_main_loop_destroy(loop);
        return std::unexpected(AudioBackendError{
            .operation = "创建 PipeWire context", .message = "返回空指针" });
    }

    auto* core = pw_context_connect(context, nullptr, 0);
    if ( core == nullptr ) {
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
        return std::unexpected(AudioBackendError{
            .operation = "连接 PipeWire", .message = "无法连接用户音频服务" });
    }

    auto* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if ( registry == nullptr ) {
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(loop);
        return std::unexpected(AudioBackendError{
            .operation = "取得 PipeWire registry", .message = "返回空指针" });
    }

    EnumerationState state{
        .loop = loop, .syncSequence = 0, .devices = {}, .error = {}
    };
    spa_hook           registryListener{};
    spa_hook           coreListener{};
    pw_registry_events registryEvents{};
    registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
    registryEvents.global  = onRegistryGlobal;
    pw_core_events coreEvents{};
    coreEvents.version = PW_VERSION_CORE_EVENTS;
    coreEvents.done    = onCoreDone;
    coreEvents.error   = onCoreError;

    pw_registry_add_listener(
        registry, &registryListener, &registryEvents, &state);
    pw_core_add_listener(core, &coreListener, &coreEvents, &state);
    state.syncSequence = pw_core_sync(core, PW_ID_CORE, 0);
    if ( state.syncSequence < 0 ) {
        state.error = "无法提交 PipeWire registry 同步";
    } else {
        pw_main_loop_run(loop);
    }

    // 监听器必须先于其代理和 context 移除，避免销毁阶段回调悬空状态。
    spa_hook_remove(&coreListener);
    spa_hook_remove(&registryListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    if ( !state.error.empty() ) {
        return std::unexpected(
            AudioBackendError{ .operation = "枚举 PipeWire 设备",
                               .message   = std::move(state.error) });
    }
    return std::move(state.devices);
}

}  // namespace

std::unique_ptr<IAudioBackend> createPipeWireBackend()
{
    return std::make_unique<PipeWireBackend>();
}

}  // namespace AudioRoads::Audio
