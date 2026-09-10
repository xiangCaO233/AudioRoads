#pragma once

#include "AudioTypes.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace AudioRoads::Core
{

using RouteId = std::uint64_t;

/// @brief 一条从采集端点流向播放端点的用户路由。
struct AudioRoute {
    /// @brief 进程内单调生成的路由标识符。
    RouteId id{};

    /// @brief 输入设备的后端稳定标识符。
    std::string sourceDeviceId;

    /// @brief 输出设备的后端稳定标识符。
    std::string sinkDeviceId;

    /// @brief 在线性域应用于该路由的增益。
    float gain{ 1.0F };

    /// @brief 静音只影响混音，不销毁路由或设备流。
    bool muted{};
};

/// @brief 路由图操作可能返回的可恢复错误。
enum class RoutingError : std::uint8_t {
    /// 请求的源 ID 不在当前设备快照中。
    SourceNotFound,
    /// 请求的目标 ID 不在当前设备快照中。
    SinkNotFound,
    /// 源端点存在，但其方向不支持采集。
    SourceCannotCapture,
    /// 目标端点存在，但其方向不支持播放。
    SinkCannotRender,
    /// 相同有向端点对已经有路由。
    DuplicateRoute,
    /// 更新操作使用的路由 ID 已不存在。
    RouteNotFound,
    /// 增益不是有限值或超出 0.0 到 4.0。
    InvalidGain,
};

/// @brief 保存设备快照和用户路由，并集中维护图不变量。
///
/// 此类型不启动音频流，也不持有平台对象。设备热插拔后，失效路由会保留，
/// 让 UI 能向用户展示断开的连接并在设备重新出现时恢复。
/// 路由只保存稳定设备 ID，不缓存会被快照替换使失效的指针。
/// 类型属于控制面且非线程安全，所有写入必须串行化。
/// 未来实时线程应消费由此图生成的独立不变执行快照。
/// 新增业务约束必须在此层实现，不得只靠 UI 禁用控件。
class RoutingGraph final
{
public:
    /// @brief 用最新发现结果替换设备快照。
    /// @details 只替换设备列表，现有路由保留以支持热插拔恢复。
    void replaceDevices(std::vector<AudioDevice> devices);

    /// @brief 返回当前设备快照。
    /// @warning 引用及其元素只在下次 replaceDevices 前保持有效。
    [[nodiscard]] const std::vector<AudioDevice>& devices() const noexcept;

    /// @brief 返回当前路由列表。
    /// @warning 引用及其元素只在下次创建或删除路由前保持有效。
    [[nodiscard]] const std::vector<AudioRoute>& routes() const noexcept;

    /// @brief 在两个兼容端点之间创建路由。
    /// @details 依次校验增益、源端存在性/方向、目标存在性/方向
    /// 以及有向设备对唯一性；任一失败都不修改图。
    /// @return 新路由 ID；失败时返回违反的图约束。
    [[nodiscard]] std::expected<RouteId, RoutingError> createRoute(
        std::string sourceDeviceId, std::string sinkDeviceId,
        float gain = 1.0F);

    /// @brief 修改现有路由的实时参数。
    /// @details 只改变增益和静音；切换端点需删除后重新创建。
    /// @return ID 不存在或增益非法时返回显式错误。
    [[nodiscard]] std::expected<void, RoutingError> updateRoute(RouteId id,
                                                                float   gain,
                                                                bool    muted);

    /// @brief 从图中移除路由。
    /// @return 命中并删除时为 true；未命中时为 false，因此操作可幂等重试。
    [[nodiscard]] bool removeRoute(RouteId id) noexcept;

    /// @brief 查找设备，未命中时返回空观察指针。
    /// @warning 指针只在下次 replaceDevices 之前有效，不得跨快照替换缓存。
    [[nodiscard]] const AudioDevice* findDevice(
        const std::string& id) const noexcept;

private:
    /// @brief 下一路由标识符，零保留为无效值。
    /// @note 仅在创建成功后递增，失败不消耗 ID。
    RouteId m_nextRouteId{ 1 };

    /// @brief 低频更新的设备发现快照。
    std::vector<AudioDevice> m_devices;

    /// @brief 按创建顺序保存的路由配置。
    std::vector<AudioRoute> m_routes;
};

/// @brief 将错误枚举转换为面向 UI 的简短中文文本。
[[nodiscard]] const char* routingErrorMessage(RoutingError error) noexcept;

}  // namespace AudioRoads::Core
