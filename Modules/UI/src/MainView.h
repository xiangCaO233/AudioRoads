#pragma once

#include "RoutingGraph.h"

#include <string>

namespace AudioRoads::UI
{

/// @brief 主界面单帧产生的低频应用动作。
struct MainViewActions {
    /// @brief 用户是否要求重新发现平台设备。
    bool refreshDevices{};
};

/// @brief 绘制设备、路由创建器和路由参数面板。
class MainView final
{
public:
    /// @brief 绘制完整工作区，并直接编辑纯业务路由图。
    /// @warning 每个显示帧调用；只允许修改内存状态，不得访问平台音频 API。
    [[nodiscard]] MainViewActions draw(Core::RoutingGraph& graph,
                                       const char*         backendName);

    /// @brief 显示最近一次平台或路由操作错误。
    void setError(std::string message);

private:
    /// @brief 设备刷新后把组合框索引收敛到有效范围。
    void normalizeSelections(const Core::RoutingGraph& graph) noexcept;

    /// @brief 当前输入设备在筛选列表中的索引。
    int m_sourceSelection{};

    /// @brief 当前输出设备在筛选列表中的索引。
    int m_sinkSelection{};

    /// @brief 创建新路由时使用的线性增益。
    float m_newRouteGain{ 1.0F };

    /// @brief 最近一次可恢复错误；成功操作会清空。
    std::string m_error;
};

}  // namespace AudioRoads::UI
