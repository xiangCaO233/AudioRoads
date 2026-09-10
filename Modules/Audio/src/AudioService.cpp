#include "AudioService.h"

#include <utility>

namespace AudioRoads::Audio
{

AudioService::AudioService() : AudioService(createPlatformAudioBackend())
{
    // 默认路径只委托编译期工厂，不在服务层重复平台探测或包含原生 API。
}

AudioService::AudioService(std::unique_ptr<IAudioBackend> backend)
    : m_backend(std::move(backend))
{
    // unique_ptr 明确后端由服务独占；注入路径也允许测试构造空值验证失败契约。
}

std::expected<void, AudioBackendError> AudioService::refreshDevices()
{
    // 注入构造允许测试传入空后端；在解引用前把组装错误转成普通控制面错误。
    if ( !m_backend ) {
        return std::unexpected(AudioBackendError{ .operation = "创建音频后端",
                                                  .message = "平台后端为空" });
    }

    // 枚举可能同步访问平台服务，只能由 Application 的低频控制面触发。
    auto devices = m_backend->enumerateDevices();
    // 平台错误保留原始操作上下文，上层统一决定展示或记录方式。
    if ( !devices ) return std::unexpected(std::move(devices.error()));

    // 只有完整枚举成功才提交快照，避免瞬时平台错误清空现有工作区。
    // RoutingGraph 负责保留指向暂时离线 ID 的路由，服务不隐式删用户配置。
    m_routingGraph.replaceDevices(std::move(*devices));
    // 移动避免复制完整字符串集合；后端 expected 随调用结束释放其空容器状态。
    return {};
}

const char* AudioService::backendName() const noexcept
{
    // 空后端仍提供稳定标签，使诊断 UI 不必复制空值判断。
    // 返回值遵循后端静态文本寿命，不创建逐帧临时字符串。
    return m_backend ? m_backend->name() : "Unavailable";
}

Core::RoutingGraph& AudioService::routingGraph() noexcept
{
    // 返回内部模型的借用引用，不复制容器；寿命不超过所属 AudioService。
    return m_routingGraph;
}

const Core::RoutingGraph& AudioService::routingGraph() const noexcept
{
    // const 重载允许只读组装代码观察同一快照，不提供后端所有权或平台句柄。
    return m_routingGraph;
}

}  // namespace AudioRoads::Audio
