#pragma once

#include <imgui.h>

namespace AudioRoads::UI
{

/// @brief 项目按钮的统一入口，后续悬浮和声音反馈只需在此扩展。
[[nodiscard]] inline bool feedbackButton(const char*   label,
                                         const ImVec2& size = ImVec2{})
{
    return ImGui::Button(label, size);
}

/// @brief 项目小按钮的统一入口。
[[nodiscard]] inline bool feedbackSmallButton(const char* label)
{
    return ImGui::SmallButton(label);
}

}  // namespace AudioRoads::UI
