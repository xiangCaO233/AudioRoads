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
    SourceNotFound,
    SinkNotFound,
    SourceCannotCapture,
    SinkCannotRender,
    DuplicateRoute,
    RouteNotFound,
    InvalidGain,
};

/// @brief 保存设备快照和用户路由，并集中维护图不变量。
///
/// 此类型不启动音频流，也不持有平台对象。设备热插拔后，失效路由会保留，
/// 让 UI 能向用户展示断开的连接并在设备重新出现时恢复。
class RoutingGraph final
{
public:
    /// @brief 用最新发现结果替换设备快照。
    void replaceDevices(std::vector<AudioDevice> devices);

    /// @brief 返回当前设备快照。
    [[nodiscard]] const std::vector<AudioDevice>& devices() const noexcept;

    /// @brief 返回当前路由列表。
    [[nodiscard]] const std::vector<AudioRoute>& routes() const noexcept;

    /// @brief 在两个兼容端点之间创建路由。
    /// @return 新路由 ID；失败时返回违反的图约束。
    [[nodiscard]] std::expected<RouteId, RoutingError> createRoute(
        std::string sourceDeviceId, std::string sinkDeviceId,
        float gain = 1.0F);

    /// @brief 修改现有路由的实时参数。
    [[nodiscard]] std::expected<void, RoutingError> updateRoute(RouteId id,
                                                                float   gain,
                                                                bool    muted);

    /// @brief 从图中移除路由。
    [[nodiscard]] bool removeRoute(RouteId id) noexcept;

    /// @brief 查找设备，未命中时返回空观察指针。
    [[nodiscard]] const AudioDevice* findDevice(
        const std::string& id) const noexcept;

private:
    /// @brief 下一路由标识符，零保留为无效值。
    RouteId m_nextRouteId{ 1 };

    /// @brief 低频更新的设备发现快照。
    std::vector<AudioDevice> m_devices;

    /// @brief 按创建顺序保存的路由配置。
    std::vector<AudioRoute> m_routes;
};

/// @brief 将错误枚举转换为面向 UI 的简短中文文本。
[[nodiscard]] const char* routingErrorMessage(RoutingError error) noexcept;

}  // namespace AudioRoads::Core
