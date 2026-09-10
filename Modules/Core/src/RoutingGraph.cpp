#include "RoutingGraph.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace AudioRoads::Core
{

void RoutingGraph::replaceDevices(std::vector<AudioDevice> devices)
{
    // 设备列表只在用户刷新或后端通知时整体替换，保持读侧接口简单稳定。
    // 路由只保存稳定设备 ID，因此端点暂时消失时仍保留配置，等待同 ID 恢复。
    // 该移动同时令旧 devices 元素地址失效，findDevice 返回值不得跨此调用保存。
    m_devices = std::move(devices);
}

const std::vector<AudioDevice>& RoutingGraph::devices() const noexcept
{
    return m_devices;
}

const std::vector<AudioRoute>& RoutingGraph::routes() const noexcept
{
    return m_routes;
}

std::expected<RouteId, RoutingError> RoutingGraph::createRoute(
    std::string sourceDeviceId, std::string sinkDeviceId, float gain)
{
    // 所有约束都在修改容器前验证，使失败不消耗 ID，也不留下半成品路由。
    // 增益同时拒绝 NaN/Inf 和范围外有限值，避免异常浮点进入后续实时快照。
    if ( !std::isfinite(gain) || gain < 0.0F || gain > 4.0F ) {
        return std::unexpected(RoutingError::InvalidGain);
    }

    const auto* source = findDevice(sourceDeviceId);
    // 先区分端点缺失和方向错误，UI 才能给出具体的可恢复原因。
    if ( source == nullptr )
        return std::unexpected(RoutingError::SourceNotFound);
    if ( !canCapture(*source) ) {
        return std::unexpected(RoutingError::SourceCannotCapture);
    }

    const auto* sink = findDevice(sinkDeviceId);
    // 目标校验与源校验对称，但保留独立错误枚举供调用方定位错误一端。
    if ( sink == nullptr ) return std::unexpected(RoutingError::SinkNotFound);
    if ( !canRender(*sink) ) {
        return std::unexpected(RoutingError::SinkCannotRender);
    }

    // 源、目标组成有序对；即使双工设备可处于任一端，路由方向仍不可互换。
    // 一个端点对只保留一条路由，增益和静音通过 updateRoute 修改。
    const auto duplicate =
        std::ranges::find_if(m_routes, [&](const AudioRoute& route) {
            return route.sourceDeviceId == sourceDeviceId &&
                   route.sinkDeviceId == sinkDeviceId;
        });
    if ( duplicate != m_routes.end() ) {
        return std::unexpected(RoutingError::DuplicateRoute);
    }

    const auto id = m_nextRouteId++;
    // ID 只在所有验证通过后递增，失败重试不会产生无意义缺口；零永不发放。
    m_routes.push_back(AudioRoute{ .id             = id,
                                   .sourceDeviceId = std::move(sourceDeviceId),
                                   .sinkDeviceId   = std::move(sinkDeviceId),
                                   .gain           = gain,
                                   .muted          = false });
    return id;
}

std::expected<void, RoutingError> RoutingGraph::updateRoute(RouteId id,
                                                            float   gain,
                                                            bool    muted)
{
    // update 只修改运行参数；端点变化必须重新创建，以复用完整端点校验。
    // 与 createRoute 共享同一数值范围，防止不同入口产生互不兼容的路由状态。
    if ( !std::isfinite(gain) || gain < 0.0F || gain > 4.0F ) {
        return std::unexpected(RoutingError::InvalidGain);
    }

    const auto route = std::ranges::find(m_routes, id, &AudioRoute::id);
    if ( route == m_routes.end() ) {
        // 热插拔不会删除路由，但显式删除后对旧 ID 的更新必须可观测地失败。
        return std::unexpected(RoutingError::RouteNotFound);
    }

    route->gain  = gain;
    route->muted = muted;
    // 两个运行参数在查找成功后连续提交；当前控制面由单线程事件循环串行调用。
    return {};
}

bool RoutingGraph::removeRoute(RouteId id) noexcept
{
    // erase_if 将“查找并删除”合并为一次遍历，尺寸变化即为调用结果。
    const auto oldSize = m_routes.size();
    std::erase_if(m_routes,
                  [id](const AudioRoute& route) { return route.id == id; });
    // 不把“未找到”当异常，布尔返回让 UI 和未来配置同步代码自行决定策略。
    return m_routes.size() != oldSize;
}

const AudioDevice* RoutingGraph::findDevice(
    const std::string& id) const noexcept
{
    // 返回非拥有指针；下一次 replaceDevices 会使它失效，调用方不可跨刷新缓存。
    const auto device = std::ranges::find(m_devices, id, &AudioDevice::id);
    // 投影查找只比较稳定 ID，不依赖可变展示名、默认标志或格式快照。
    return device == m_devices.end() ? nullptr : &*device;
}

const char* routingErrorMessage(RoutingError error) noexcept
{
    // 文本仅用于当前 UI 展示，不是稳定序列化格式；持久化应保存枚举映射值。
    // switch 不设 default，让编译器告警能提示新增枚举值遗漏；末尾处理损坏值。
    switch ( error ) {
    case RoutingError::SourceNotFound: return "输入设备不存在";
    case RoutingError::SinkNotFound: return "输出设备不存在";
    case RoutingError::SourceCannotCapture: return "源设备不支持采集";
    case RoutingError::SinkCannotRender: return "目标设备不支持播放";
    case RoutingError::DuplicateRoute: return "该设备对已经存在路由";
    case RoutingError::RouteNotFound: return "路由不存在";
    case RoutingError::InvalidGain: return "增益必须位于 0.0 到 4.0";
    }
    // 防御来自越界转换的枚举值，同时保持函数 noexcept 且始终有可展示文本。
    return "未知路由错误";
}

}  // namespace AudioRoads::Core
