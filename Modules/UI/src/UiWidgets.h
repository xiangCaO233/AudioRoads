#pragma once

#include <imgui.h>

namespace AudioRoads::UI
{

/// @brief 项目按钮的统一入口，后续悬浮和声音反馈只需在此扩展。
/// @return 仅表示当前帧被激活，不代表调用方的业务动作已经成功。
/// @note 保留 ImGui 的 label 生命周期、零尺寸和 ID 栈语义，不保存业务状态。
[[nodiscard]] inline bool feedbackButton(const char*   label,
                                         const ImVec2& size = ImVec2{})
{
    return ImGui::Button(label, size);
}

/// @brief 项目小按钮的统一入口。
/// @note 若未来接入交互音效，只能发出非阻塞事件，不能直接操作音频设备。
[[nodiscard]] inline bool feedbackSmallButton(const char* label)
{
    return ImGui::SmallButton(label);
}

}  // namespace AudioRoads::UI
