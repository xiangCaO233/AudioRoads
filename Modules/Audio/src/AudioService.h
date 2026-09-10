#pragma once

#include "AudioBackendFactory.h"
#include "RoutingGraph.h"

#include <expected>
#include <memory>

namespace AudioRoads::Audio
{

/// @brief 连接平台设备发现与纯业务路由图的应用服务。
///
/// UI 通过此类获取后端状态和路由图，不直接接触平台 API。
/// 服务不启动自有线程，调用时序由 Application 串行管理。
/// 可变路由图属于控制面；未来实时处理必须消费独立不变快照。
class AudioService final
{
public:
    /// @brief 使用当前平台默认后端创建服务。
    AudioService();

    /// @brief 注入后端，主要用于测试或未来的离线设备提供器。
    /// @details 转入的 unique_ptr 成为服务独占所有的平台资源边界。
    explicit AudioService(std::unique_ptr<IAudioBackend> backend);

    /// @brief 刷新设备快照；失败时保留上一份成功结果。
    /// @details 仅当后端返回完整快照时才替换图内设备，
    /// 因此短暂枚举故障不会清空用户已见状态。
    /// 该操作可能同步访问系统服务，禁止从实时回调调用。
    [[nodiscard]] std::expected<void, AudioBackendError> refreshDevices();

    /// @brief 返回平台后端名称。
    /// @note 该值只用于诊断展示，不能驱动业务分支。
    [[nodiscard]] const char* backendName() const noexcept;

    /// @brief 返回可由 UI 编辑的路由图。
    [[nodiscard]] Core::RoutingGraph& routingGraph() noexcept;

    /// @brief 返回只读路由图。
    [[nodiscard]] const Core::RoutingGraph& routingGraph() const noexcept;

private:
    /// @brief 平台资源的唯一所有者。
    std::unique_ptr<IAudioBackend> m_backend;

    /// @brief 与平台句柄解耦的设备及路由状态。
    /// @note 声明在 m_backend 之后，析构时先销毁纯值状态。
    Core::RoutingGraph m_routingGraph;
};

}  // namespace AudioRoads::Audio
