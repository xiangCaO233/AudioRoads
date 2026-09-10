#pragma once

#include "AudioTypes.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace AudioRoads::Core
{

using RouteId = std::uint64_t;

/// @brief 一条从采集来源流向数据消费目标的用户路由。
struct AudioRoute {
    /// @brief 进程内单调生成的路由标识符。
    /// @invariant 零不分配给真实路由，也不会在删除后复用。
    RouteId id{};

    /// @brief 输入设备或应用输出来源的稳定标识符。
    /// @note 端点离线时仍保留原值，用于后续自动重连。
    std::string sourceId;

    /// @brief 播放设备或虚拟麦克风目标的稳定标识符。
    /// @note 与 sourceId 组成图内唯一的有向端点对。
    std::string targetId;

    /// @brief 在线性域应用于该路由的增益。
    float gain{ 1.0F };

    /// @brief 静音只影响数据贡献，不关闭来源、目标或删除路由。
    bool muted{};
};

/// @brief 路由图操作可能返回的可恢复错误。
enum class RoutingError : std::uint8_t {
    /// 请求的来源 ID 不在当前快照中。
    SourceNotFound,
    /// 请求的目标 ID 不在当前快照中。
    TargetNotFound,
    /// 相同有向来源和目标已经有路由。
    DuplicateRoute,
    /// 更新操作使用的路由 ID 已不存在。
    RouteNotFound,
    /// 增益不是有限值或超出 0.0 到 4.0。
    InvalidGain,
};

/// @brief 保存端点快照和用户路由，并集中维护图不变量。
///
/// 路由只连接纯数据来源与消费者，不根据操作系统的“输入/输出设备”名称推断
/// 数据方向。端点热插拔后失效路由继续保留，以便同一稳定 ID 恢复时重新接通。
/// 类型属于非线程安全控制面；实时线程必须消费另行生成的不可变执行快照。
/// 来源允许一对多扇出，目标允许多对一汇入；唯一性只约束相同来源/目标有向对。
/// 图不负责打开平台流、重采样或执行混音，因而任何操作都不会进入设备回调。
class RoutingGraph final
{
public:
    /// @brief 原子替换平台发现的来源与目标快照。
    /// @details 现有路由不随端点消失而删除，所有旧端点观察指针都会失效。
    void replaceEndpoints(AudioEndpointSnapshot snapshot);

    /// @brief 返回当前来源快照。
    /// @warning 引用及其元素只在下次 replaceEndpoints 前保持有效。
    [[nodiscard]] const std::vector<AudioSource>& sources() const noexcept;

    /// @brief 返回当前目标快照。
    /// @warning 引用及其元素只在下次 replaceEndpoints 前保持有效。
    [[nodiscard]] const std::vector<AudioTarget>& targets() const noexcept;

    /// @brief 返回当前路由列表。
    /// @warning 引用及其元素只在下次创建或删除路由前保持有效。
    [[nodiscard]] const std::vector<AudioRoute>& routes() const noexcept;

    /// @brief 在来源与数据消费目标之间创建有向路由。
    /// @details 依次校验增益、来源存在性、目标存在性及端点对唯一性；
    /// 任一失败都不修改图，也不消耗新路由 ID。
    [[nodiscard]] std::expected<RouteId, RoutingError> createRoute(
        std::string sourceId, std::string targetId, float gain = 1.0F);

    /// @brief 修改现有路由的实时参数。
    /// @details 只改变增益和静音；切换端点需删除后重新创建。
    [[nodiscard]] std::expected<void, RoutingError> updateRoute(RouteId id,
                                                                float   gain,
                                                                bool    muted);

    /// @brief 从图中移除路由。
    /// @return 命中并删除时为 true；未命中时为 false，可幂等重试。
    [[nodiscard]] bool removeRoute(RouteId id) noexcept;

    /// @brief 查找来源，未命中时返回空观察指针。
    /// @warning 指针不得跨 replaceEndpoints 缓存。
    [[nodiscard]] const AudioSource* findSource(
        const std::string& id) const noexcept;

    /// @brief 查找目标，未命中时返回空观察指针。
    /// @warning 指针不得跨 replaceEndpoints 缓存。
    [[nodiscard]] const AudioTarget* findTarget(
        const std::string& id) const noexcept;

private:
    /// @brief 下一路由标识符，零保留为无操作哨兵。
    RouteId m_nextRouteId{ 1 };

    /// @brief 低频整体替换的来源快照。
    std::vector<AudioSource> m_sources;

    /// @brief 低频整体替换的数据消费目标快照。
    std::vector<AudioTarget> m_targets;

    /// @brief 按创建顺序保存的纯数据流路由配置。
    std::vector<AudioRoute> m_routes;
};

/// @brief 将错误枚举转换为面向 UI 的简短中文文本。
[[nodiscard]] const char* routingErrorMessage(RoutingError error) noexcept;

}  // namespace AudioRoads::Core
