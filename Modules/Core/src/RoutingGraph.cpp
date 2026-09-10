#include "RoutingGraph.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace AudioRoads::Core
{

void RoutingGraph::replaceDevices(std::vector<AudioDevice> devices)
{
    // 设备列表只在用户刷新或后端通知时整体替换，保持读侧接口简单稳定。
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
    if ( !std::isfinite(gain) || gain < 0.0F || gain > 4.0F ) {
        return std::unexpected(RoutingError::InvalidGain);
    }

    const auto* source = findDevice(sourceDeviceId);
    if ( source == nullptr )
        return std::unexpected(RoutingError::SourceNotFound);
    if ( !canCapture(*source) ) {
        return std::unexpected(RoutingError::SourceCannotCapture);
    }

    const auto* sink = findDevice(sinkDeviceId);
    if ( sink == nullptr ) return std::unexpected(RoutingError::SinkNotFound);
    if ( !canRender(*sink) ) {
        return std::unexpected(RoutingError::SinkCannotRender);
    }

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
    if ( !std::isfinite(gain) || gain < 0.0F || gain > 4.0F ) {
        return std::unexpected(RoutingError::InvalidGain);
    }

    const auto route = std::ranges::find(m_routes, id, &AudioRoute::id);
    if ( route == m_routes.end() ) {
        return std::unexpected(RoutingError::RouteNotFound);
    }

    route->gain  = gain;
    route->muted = muted;
    return {};
}

bool RoutingGraph::removeRoute(RouteId id) noexcept
{
    const auto oldSize = m_routes.size();
    std::erase_if(m_routes,
                  [id](const AudioRoute& route) { return route.id == id; });
    return m_routes.size() != oldSize;
}

const AudioDevice* RoutingGraph::findDevice(
    const std::string& id) const noexcept
{
    const auto device = std::ranges::find(m_devices, id, &AudioDevice::id);
    return device == m_devices.end() ? nullptr : &*device;
}

const char* routingErrorMessage(RoutingError error) noexcept
{
    switch ( error ) {
    case RoutingError::SourceNotFound: return "输入设备不存在";
    case RoutingError::SinkNotFound: return "输出设备不存在";
    case RoutingError::SourceCannotCapture: return "源设备不支持采集";
    case RoutingError::SinkCannotRender: return "目标设备不支持播放";
    case RoutingError::DuplicateRoute: return "该设备对已经存在路由";
    case RoutingError::RouteNotFound: return "路由不存在";
    case RoutingError::InvalidGain: return "增益必须位于 0.0 到 4.0";
    }
    return "未知路由错误";
}

}  // namespace AudioRoads::Core
