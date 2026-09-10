#pragma once

#include "RoutingGraph.h"

#include <string>

namespace AudioRoads::UI
{

/// @brief 主界面单帧产生的低频应用动作，由 Main 层在帧提交后执行。
struct MainViewActions {
    /// @brief 用户是否要求重新发现平台设备。
    bool refreshDevices{};
};

/// @brief 绘制设备、路由创建器和路由参数面板。
///
/// 视图只保留选择索引、编辑草稿和错误文本，不持有业务对象或设备指针。平台
/// 操作通过 MainViewActions 返回给 Application；新增业务约束应进入 Core，
/// 不得只依靠控件禁用来维持模型有效性。
class MainView final
{
public:
    /// @brief 绘制完整工作区，并直接编辑纯业务路由图。
    /// @return 当前帧产生的低频动作；调用方应在 ImGui 帧结束后处理。
    /// @warning 每个显示帧调用；不得访问平台音频 API、文件系统或阻塞等待。
    [[nodiscard]] MainViewActions draw(Core::RoutingGraph& graph,
                                       const char*         backendName);

    /// @brief 显示最近一次平台或路由操作错误。
    /// @details 文本按值进入视图并跨帧保存；传入空串可清除已恢复故障。
    void setError(std::string message);

private:
    /// @brief 设备快照替换后把筛选列表索引收敛到新的有效范围。
    void normalizeSelections(const Core::RoutingGraph& graph) noexcept;

    /// @brief 当前输入设备在筛选列表中的索引。
    /// @note 不是 graph.devices() 的原始下标，设备刷新后必须重新收敛。
    int m_sourceSelection{};

    /// @brief 当前输出设备在筛选列表中的索引。
    /// @note 不缓存对应设备地址，避免快照替换后形成悬空指针。
    int m_sinkSelection{};

    /// @brief 创建新路由时使用的草稿线性增益，仅在用户确认时提交。
    float m_newRouteGain{ 1.0F };

    /// @brief 最近一次可恢复错误；成功操作会清空。
    std::string m_error;
};

}  // namespace AudioRoads::UI
