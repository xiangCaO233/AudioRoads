#include "AudioService.h"

#include <utility>

namespace AudioRoads::Audio
{

AudioService::AudioService() : AudioService(createPlatformAudioBackend()) {}

AudioService::AudioService(std::unique_ptr<IAudioBackend> backend)
    : m_backend(std::move(backend))
{
}

std::expected<void, AudioBackendError> AudioService::refreshDevices()
{
    if ( !m_backend ) {
        return std::unexpected(AudioBackendError{ .operation = "创建音频后端",
                                                  .message = "平台后端为空" });
    }

    auto devices = m_backend->enumerateDevices();
    if ( !devices ) return std::unexpected(std::move(devices.error()));

    // 只有完整枚举成功才提交快照，避免瞬时平台错误清空现有工作区。
    m_routingGraph.replaceDevices(std::move(*devices));
    return {};
}

const char* AudioService::backendName() const noexcept
{
    return m_backend ? m_backend->name() : "Unavailable";
}

Core::RoutingGraph& AudioService::routingGraph() noexcept
{
    return m_routingGraph;
}

const Core::RoutingGraph& AudioService::routingGraph() const noexcept
{
    return m_routingGraph;
}

}  // namespace AudioRoads::Audio
