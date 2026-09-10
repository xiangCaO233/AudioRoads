#pragma once

#include "RoutingGraph.h"

#include <string>

namespace AudioRoads::UI
{

/// @brief 主界面单帧产生的低频应用动作，由 Main 层在帧提交后执行。
struct MainViewActions {
    /// @brief 用户是否要求重新发现音频来源与目标。
    bool refreshEndpoints{};
};

/// @brief 绘制端点节点画布、连接线和路由参数面板。
///
/// 视图只保留待连接来源 ID、编辑草稿和错误文本，不持有业务对象或设备指针。平台
/// 操作通过 MainViewActions 返回给 Application；新增业务约束应进入 Core，
/// 不得只依靠控件禁用来维持模型有效性。
/// 来源与目标以方块展示，已有 AudioRoute 以有向线展示；多条线进入同一目标
/// 表示混音。节点位置目前由端点快照顺序自动布局，不作为项目配置持久化。
class MainView final
{
public:
    /// @brief 绘制完整工作区，并直接编辑纯业务路由图。
    /// @return 当前帧产生的低频动作；调用方应在 ImGui 帧结束后处理。
    /// @warning 每个显示帧调用；不得访问平台音频 API、文件系统或阻塞等待。
    /// @note 连接操作直接修改 RoutingGraph，但只涉及控制面容器和标量。
    [[nodiscard]] MainViewActions draw(Core::RoutingGraph& graph,
                                       const char*         backendName);

    /// @brief 显示最近一次平台或路由操作错误。
    /// @details 文本按值进入视图并跨帧保存；传入空串可清除已恢复故障。
    void setError(std::string message);

private:
    /// @brief 端点刷新后清理已经离线的待连接来源。
    void normalizePendingConnection(const Core::RoutingGraph& graph);

    /// @brief 用户在节点画布中选中、等待连接到目标的来源稳定 ID。
    /// @note 保存 ID 而非容器索引，刷新导致排序变化时不会误连其他端点。
    std::string m_pendingSourceId;

    /// @brief 创建新路由时使用的草稿线性增益，仅在用户确认时提交。
    float m_newRouteGain{ 1.0F };

    /// @brief 最近一次可恢复错误；成功操作会清空。
    std::string m_error;
};

}  // namespace AudioRoads::UI
