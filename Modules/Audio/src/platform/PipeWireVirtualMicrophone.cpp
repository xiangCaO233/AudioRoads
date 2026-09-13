#include "PipeWireVirtualMicrophone.h"

#include <pipewire/impl-module.h>
#include <pipewire/pipewire.h>

#include <spa/utils/hook.h>

#include <atomic>
#include <expected>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace AudioRoads::Audio
{
namespace
{

// loopback 的两个流共享 48 kHz 双声道格式。内部输入明确关闭会话管理器自动连接，
// 否则无路由时它可能擅自接到默认麦克风，破坏“只包含用户指定音源”的契约。
// 公开端保持 Audio/Source，让遵循 PipeWire/PulseAudio
// 的其他应用按普通麦克风发现。
// remote.name 采用 PipeWire 工具相同的 manager/default 回退，标准桌面服务在
// manager socket 不存在时仍可连接 pipewire-0；模块不会自行管理系统守护进程。
// node.name 是跨 registry 刷新的匹配协议，改名必须同步头文件常量和枚举逻辑。
// 全局格式同时约束内部输入与公开 Source，避免两端独立协商产生隐藏通道差异。
// capture 在 loopback 术语中是消费端，数据方向正好对应 AudioRoads target。
// Stream/Input/Audio 让注入端保持普通流身份，不进入桌面的播放设备选择列表。
// node.autoconnect=false 是隔离关键：无路由时不得把默认麦克风偷偷注入输出。
// node.dont-reconnect=true 防止断线后由策略层把内部流迁移到任意新来源。
// node.passive=true 让空闲内部支路不争夺图驱动权，公开 Source 仍可被外部拉取。
// playback 在 loopback 术语中产出数据，标为 Audio/Source 后才是系统录音端点。
constexpr char VIRTUAL_MICROPHONE_MODULE_ARGUMENTS[] = R"(
{
    remote.name = [ pipewire-0-manager, pipewire-0 ]
    node.name = audioroads.virtual-microphone
    node.description = "AudioRoads Virtual Microphone"
    audio.rate = 48000
    audio.channels = 2
    audio.position = [ FL FR ]
    capture.props = {
        node.name = audioroads.virtual-microphone.input
        node.description = "AudioRoads Virtual Microphone Input"
        media.class = Stream/Input/Audio
        node.autoconnect = false
        node.dont-reconnect = true
        node.passive = true
        node.virtual = true
    }
    playback.props = {
        node.name = audioroads.virtual-microphone
        node.description = "AudioRoads Virtual Microphone"
        media.class = Audio/Source
        node.virtual = true
    }
}
)";

/// @brief 生成统一的虚拟麦克风创建错误。
/// @details 仅控制面调用并拥有 message；模块的数据回调从不进入该函数。
[[nodiscard]] AudioBackendError publicationError(std::string message)
{
    return AudioBackendError{ .operation = "创建 PipeWire 虚拟麦克风",
                              .message   = std::move(message) };
}

}  // namespace

struct PipeWireVirtualMicrophone::Implementation {
    /// @brief 最终安全网；退出循环并等待线程后才销毁 listener 借用的本对象。
    ~Implementation() { stop(); }

    /// @brief 首次调用建立节点，节点仍存活时不重复创建。
    [[nodiscard]] std::expected<void, AudioBackendError> ensurePublished();

    /// @brief 幂等停止当前代模块并逆序释放上下文对象树。
    void stop() noexcept;

    /// @brief 模块因服务错误自毁时清除句柄并结束所属主循环。
    /// @param userData 固定地址的 Implementation，由 module listener 借用。
    static void moduleDestroyed(void* userData) noexcept;

    /// @brief 驱动 loopback 控制事件与其内部实时数据循环的主循环。
    pw_main_loop* loop{};

    /// @brief 模块和默认远端连接的本地所有者，寿命短于 loop。
    pw_context* context{};

    /// @brief 创建两个流的本地模块；可能由错误回调先行销毁并置空。
    pw_impl_module* module{};

    /// @brief 监听模块自毁，确保控制面不会再次释放失效句柄。
    spa_hook moduleListener{};

    /// @brief main-loop 的唯一运行线程；stop 先 quit 再 join。
    std::jthread loopThread;

    /// @brief 控制线程查询的发布状态；模块回调以原子方式撤销。
    std::atomic_bool published{};
};

void PipeWireVirtualMicrophone::Implementation::moduleDestroyed(
    void* userData) noexcept
{
    auto& implementation = *static_cast<Implementation*>(userData);
    // listener 与 module 同属主循环线程；先移除钩子，避免后续销毁阶段二次回调。
    spa_hook_remove(&implementation.moduleListener);
    implementation.module = nullptr;
    implementation.published.store(false, std::memory_order_release);
    // 模块已不可能恢复，结束循环让下一次 ensurePublished 可以完整重建对象树。
    pw_main_loop_quit(implementation.loop);
}

std::expected<void, AudioBackendError>
PipeWireVirtualMicrophone::Implementation::ensurePublished()
{
    // 正常运行路径只做一次无锁读取，不影响枚举的 registry round-trip。
    if ( published.load(std::memory_order_acquire) ) return {};

    // 上一代可能因 PipeWire 服务重启自行结束；先 join 并回收所有残留对象。
    // stop 完成后所有成员都回到空值，下方失败分支可继续复用同一实例。
    stop();
    loop = pw_main_loop_new(nullptr);
    if ( loop == nullptr ) {
        // loop 是对象树根，此处分配失败时没有子对象需要清理。
        return std::unexpected(publicationError("无法创建主循环"));
    }

    context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0U);
    if ( context == nullptr ) {
        // context 借用 pw_loop，统一 stop 会保留先子后父的销毁顺序。
        stop();
        return std::unexpected(publicationError("无法创建 context"));
    }

    // 模块在本地 context 中建立 loopback 两端并把节点导出到默认用户会话。
    // 参数由编译期常量拥有，pw_context_load_module 在返回前完成解析。
    module = pw_context_load_module(context,
                                    "libpipewire-module-loopback",
                                    VIRTUAL_MICROPHONE_MODULE_ARGUMENTS,
                                    nullptr);
    if ( module == nullptr ) {
        // 加载失败尚未注册 listener 或启动线程，stop 只回收 context 与 loop。
        stop();
        return std::unexpected(
            publicationError("无法加载 libpipewire-module-loopback"));
    }

    // listener 保存事件表指针而不是复制栈对象，因此表必须具有静态寿命。
    static const pw_impl_module_events events{
        .version     = PW_VERSION_IMPL_MODULE_EVENTS,
        .destroy     = moduleDestroyed,
        .free        = nullptr,
        .initialized = nullptr,
        .registered  = nullptr,
    };
    pw_impl_module_add_listener(module, &moduleListener, &events, this);

    // 模块已同步提交两个 pw_stream 的连接请求；循环线程随后分发远端状态和 PCM。
    // 线程捕获原生 loop 值，不访问可能在 join 后被清理的 owning 成员。
    // published 在线程启动前发布，避免快速枚举误判并重复构造第二组同名节点。
    // 远端若随后拒绝内部流，loopback 会自毁，listener 会原子撤销发布状态。
    auto* publishedLoop = loop;
    published.store(true, std::memory_order_release);
    loopThread = std::jthread([publishedLoop] {
        static_cast<void>(pw_main_loop_run(publishedLoop));
    });
    return {};
}

void PipeWireVirtualMicrophone::Implementation::stop() noexcept
{
    // quit 可在循环尚未 run 或已因 moduleDestroyed 退出时幂等调用。
    // jthread 的 stop_token 无法结束 pw_main_loop_run，必须使用 PipeWire quit。
    if ( loop != nullptr ) pw_main_loop_quit(loop);
    // join 保证 listener 不再并发写 module，之后才能判断剩余所有权。
    if ( loopThread.joinable() ) loopThread.join();

    // join 后模块回调不再并发修改句柄；仍非空说明这是主动关闭路径。
    if ( module != nullptr ) {
        pw_impl_module_destroy(module);
        // destroy 回调会移除 listener 并把 module 置空；显式赋值保护不同 ABI
        // 实现不触发通知的降级路径。
        module = nullptr;
    }
    if ( context != nullptr ) {
        // context 销毁其余本地资源，必须先于它借用的主循环。
        // 模块已释放，避免隐式清理时 listener 访问正在析构的成员。
        pw_context_destroy(context);
        context = nullptr;
    }
    if ( loop != nullptr ) {
        pw_main_loop_destroy(loop);
        loop = nullptr;
    }
    published.store(false, std::memory_order_release);
}

PipeWireVirtualMicrophone::PipeWireVirtualMicrophone()
    : m_implementation(std::make_unique<Implementation>())
{
    // 延迟发布允许后端构造保持无失败接口，首次枚举通过 expected 报告服务错误。
}

PipeWireVirtualMicrophone::~PipeWireVirtualMicrophone() = default;

std::expected<void, AudioBackendError>
PipeWireVirtualMicrophone::ensurePublished()
{
    return m_implementation->ensurePublished();
}

}  // namespace AudioRoads::Audio
