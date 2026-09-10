#pragma once

#include "AudioBackendFactory.h"
#include "RoutingGraph.h"

#include <expected>
#include <memory>

namespace AudioRoads::Audio
{

/// @brief 连接平台设备发现与纯业务路由图的应用服务。
class AudioService final
{
public:
    /// @brief 使用当前平台默认后端创建服务。
    AudioService();

    /// @brief 注入后端，主要用于测试或未来的离线设备提供器。
    explicit AudioService(std::unique_ptr<IAudioBackend> backend);

    /// @brief 刷新设备快照；失败时保留上一份成功结果。
    [[nodiscard]] std::expected<void, AudioBackendError> refreshDevices();

    /// @brief 返回平台后端名称。
    [[nodiscard]] const char* backendName() const noexcept;

    /// @brief 返回可由 UI 编辑的路由图。
    [[nodiscard]] Core::RoutingGraph& routingGraph() noexcept;

    /// @brief 返回只读路由图。
    [[nodiscard]] const Core::RoutingGraph& routingGraph() const noexcept;

private:
    /// @brief 平台资源的唯一所有者。
    std::unique_ptr<IAudioBackend> m_backend;

    /// @brief 与平台句柄解耦的设备及路由状态。
    Core::RoutingGraph m_routingGraph;
};

}  // namespace AudioRoads::Audio
