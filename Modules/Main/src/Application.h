#pragma once

#include "AppWindow.h"
#include "AudioService.h"
#include "MainView.h"

namespace AudioRoads
{

/// @brief 组装平台音频服务和桌面 UI 的进程级应用对象。
class Application final
{
public:
    /// @brief 初始化客户端并运行到用户关闭窗口。
    [[nodiscard]] int run();

private:
    /// @brief 执行低频设备发现并把错误转换为 UI 状态。
    void refreshDevices();

    /// @brief 平台设备和路由业务服务。
    Audio::AudioService m_audioService;

    /// @brief 桌面窗口和 ImGui 生命周期所有者。
    UI::AppWindow m_window;

    /// @brief 无平台 API 依赖的主工作区视图。
    UI::MainView m_mainView;
};

}  // namespace AudioRoads
