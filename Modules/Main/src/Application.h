#pragma once

#include "AppWindow.h"
#include "AudioService.h"
#include "MainView.h"

namespace AudioRoads
{

/// @brief 组装平台音频服务和桌面 UI 的进程级应用对象。
///
/// Main 层只协调生命周期和低频动作，不解释设备类型、路由约束或绘制控件。
/// 成员的声明顺序保证构造时先建立业务服务，析构时先释放视图和窗口；未来若
/// 引入工作线程，也必须在对象析构前完成停止和 join。
class Application final
{
public:
    /// @brief 初始化窗口、取得首份设备快照，并运行到用户关闭窗口。
    /// @return 窗口初始化失败时返回非零值；设备发现失败会留在 UI 中供重试。
    [[nodiscard]] int run();

private:
    /// @brief 执行低频设备发现并把可恢复错误转换为 UI 状态。
    /// @note 必须在帧提交后调用，避免平台 API 调用穿插在 ImGui 绘制栈中。
    void refreshEndpoints();

    /// @brief 平台设备和路由业务服务。
    Audio::AudioService m_audioService;

    /// @brief 桌面窗口和 ImGui 生命周期所有者。
    UI::AppWindow m_window;

    /// @brief 无平台 API 依赖的主工作区视图。
    UI::MainView m_mainView;
};

}  // namespace AudioRoads
