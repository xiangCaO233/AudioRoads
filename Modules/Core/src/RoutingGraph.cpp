#include "RoutingGraph.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace AudioRoads::Core
{

void RoutingGraph::replaceEndpoints(AudioEndpointSnapshot snapshot)
{
    // 两类容器来自同一次后端枚举，连续移动提交可防止 UI 混用两代快照。
    // 路由只保存 ID，因此端点暂时消失时仍保留用户配置。
    m_sources = std::move(snapshot.sources);
    m_targets = std::move(snapshot.targets);
    ++m_revision;
}

const std::vector<AudioSource>& RoutingGraph::sources() const noexcept
{
    // 控制面调用者借用容器；不复制可避免 UI 每帧重复分配端点字符串。
    return m_sources;
}

const std::vector<AudioTarget>& RoutingGraph::targets() const noexcept
{
    // 与 sources 属于同一代快照，只有 replaceEndpoints 才会整体替换其存储。
    return m_targets;
}

const std::vector<AudioRoute>& RoutingGraph::routes() const noexcept
{
    // 路由顺序同时作为 UI 稳定展示顺序，不按在线状态或增益隐式重排。
    return m_routes;
}

std::uint64_t RoutingGraph::revision() const noexcept
{
    // 版本只在串行控制面读写；实时线程不得直接观察 RoutingGraph。
    return m_revision;
}

std::expected<RouteId, RoutingError> RoutingGraph::createRoute(
    std::string sourceId, std::string targetId, float gain)
{
    // 所有约束都在修改容器前验证，失败不会留下半成品或消耗路由 ID。
    if ( !std::isfinite(gain) || gain < 0.0F || gain > 4.0F ) {
        return std::unexpected(RoutingError::InvalidGain);
    }

    if ( findSource(sourceId) == nullptr ) {
        // 空 ID 也通过普通未找到路径处理，避免为表现相同的输入增加特殊错误。
        return std::unexpected(RoutingError::SourceNotFound);
    }
    if ( findTarget(targetId) == nullptr ) {
        // 来源已验证但尚未修改任何成员，目标失败仍保持事务性。
        return std::unexpected(RoutingError::TargetNotFound);
    }

    // 来源与目标组成有序对；同一应用可路由到多个目标，同一目标也可接收多源。
    const auto duplicate =
        std::ranges::find_if(m_routes, [&](const AudioRoute& route) {
            return route.sourceId == sourceId && route.targetId == targetId;
        });
    if ( duplicate != m_routes.end() ) {
        return std::unexpected(RoutingError::DuplicateRoute);
    }

    const auto id = m_nextRouteId++;
    // ID 在整个进程寿命内不复用，删除旧边后 UI 的延迟动作不会命中新边。
    // 实际溢出在产品寿命内不可达；零作为 UI 删除动作哨兵始终保留。
    m_routes.push_back(AudioRoute{ .id       = id,
                                   .sourceId = std::move(sourceId),
                                   .targetId = std::move(targetId),
                                   .gain     = gain,
                                   .muted    = false });
    ++m_revision;
    return id;
}

std::expected<void, RoutingError> RoutingGraph::updateRoute(RouteId id,
                                                            float   gain,
                                                            bool    muted)
{
    // create/update 共用同一数值范围，避免非 UI 入口产生实时侧无法处理的状态。
    if ( !std::isfinite(gain) || gain < 0.0F || gain > 4.0F ) {
        return std::unexpected(RoutingError::InvalidGain);
    }

    const auto route = std::ranges::find(m_routes, id, &AudioRoute::id);
    if ( route == m_routes.end() ) {
        // 热插拔不删除路由，因此找不到只可能来自已删除或无效的路由 ID。
        return std::unexpected(RoutingError::RouteNotFound);
    }

    // 端点不可通过 update 改写，确保所有连接变化重新经过完整存在性校验。
    route->gain  = gain;
    route->muted = muted;
    ++m_revision;
    return {};
}

bool RoutingGraph::removeRoute(RouteId id) noexcept
{
    const auto oldSize = m_routes.size();
    // erase_if 保留其他路由的相对顺序；ID 唯一性保证最多删除一个元素。
    std::erase_if(m_routes,
                  [id](const AudioRoute& route) { return route.id == id; });
    const auto removed = m_routes.size() != oldSize;
    if ( removed ) ++m_revision;
    return removed;
}

const AudioSource* RoutingGraph::findSource(
    const std::string& id) const noexcept
{
    // 返回借用指针，下一次完整快照替换会使地址失效。
    const auto source = std::ranges::find(m_sources, id, &AudioSource::id);
    return source == m_sources.end() ? nullptr : &*source;
}

const AudioTarget* RoutingGraph::findTarget(
    const std::string& id) const noexcept
{
    // 目标类别前缀属于 ID 本身，因此无需在查找后再次判断目标 kind。
    const auto target = std::ranges::find(m_targets, id, &AudioTarget::id);
    return target == m_targets.end() ? nullptr : &*target;
}

const char* routingErrorMessage(RoutingError error) noexcept
{
    // 文本只面向当前 UI，不作为序列化协议；持久化状态必须保存枚举或业务字段。
    switch ( error ) {
    case RoutingError::SourceNotFound: return "音频来源不存在";
    case RoutingError::TargetNotFound: return "音频目标不存在";
    case RoutingError::DuplicateRoute: return "该来源和目标之间已经存在路由";
    case RoutingError::RouteNotFound: return "路由不存在";
    case RoutingError::InvalidGain: return "增益必须位于 0.0 到 4.0";
    }
    return "未知路由错误";
}

}  // namespace AudioRoads::Core
