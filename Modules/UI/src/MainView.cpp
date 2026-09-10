#include "MainView.h"

#include "AudioTypes.h"
#include "UiWidgets.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace AudioRoads::UI
{
namespace
{

/// @brief 返回设备方向的短标签。
[[nodiscard]] const char* flowName(Core::DeviceFlow flow) noexcept
{
    switch ( flow ) {
    case Core::DeviceFlow::Input: return "Input";
    case Core::DeviceFlow::Output: return "Output";
    case Core::DeviceFlow::Duplex: return "Duplex";
    }
    return "Unknown";
}

using DevicePredicate = bool (*)(const Core::AudioDevice&) noexcept;

/// @brief 统计满足方向约束的设备，不在显示帧中构造临时容器。
[[nodiscard]] std::size_t countDevices(
    const std::vector<Core::AudioDevice>& devices, DevicePredicate predicate)
{
    return static_cast<std::size_t>(std::ranges::count_if(devices, predicate));
}

/// @brief 在筛选视图中查找第 index 个设备，返回稳定观察指针。
[[nodiscard]] const Core::AudioDevice* nthDevice(
    const std::vector<Core::AudioDevice>& devices, DevicePredicate predicate,
    int index) noexcept
{
    for ( const auto& device : devices ) {
        if ( !predicate(device) ) continue;
        if ( index-- == 0 ) return &device;
    }
    return nullptr;
}

/// @brief 绘制设备选择组合框，列表为空时保持禁用状态。
void deviceCombo(const char*                           label,
                 const std::vector<Core::AudioDevice>& devices,
                 DevicePredicate predicate, int& selection)
{
    const auto* selectedDevice = nthDevice(devices, predicate, selection);
    const auto* preview        = selectedDevice == nullptr
                                     ? "No compatible device"
                                     : selectedDevice->name.c_str();
    if ( ImGui::BeginCombo(label, preview) ) {
        int index{};
        for ( const auto& device : devices ) {
            if ( !predicate(device) ) continue;
            const bool selected = index == selection;
            if ( ImGui::Selectable(device.name.c_str(), selected) ) {
                selection = index;
            }
            if ( selected ) ImGui::SetItemDefaultFocus();
            ++index;
        }
        ImGui::EndCombo();
    }
}

}  // namespace

MainViewActions MainView::draw(Core::RoutingGraph& graph,
                               const char*         backendName)
{
    MainViewActions actions;
    normalizeSelections(graph);

    const auto sourceCount = countDevices(graph.devices(), Core::canCapture);
    const auto sinkCount   = countDevices(graph.devices(), Core::canRender);

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos);
    ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize);
    constexpr auto windowFlags = ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("AudioRoads Workspace###MainWorkspace", nullptr, windowFlags);

    ImGui::TextUnformatted("AudioRoads");
    ImGui::SameLine();
    ImGui::TextDisabled("Native backend: %s", backendName);
    ImGui::SameLine();
    if ( feedbackButton("Refresh devices") ) actions.refreshDevices = true;

    if ( !m_error.empty() ) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0F, 0.4F, 0.35F, 1.0F });
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::SeparatorText("Devices");
    if ( ImGui::BeginTable("DevicesTable",
                           6,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_Resizable) ) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Flow");
        ImGui::TableSetupColumn("Inputs");
        ImGui::TableSetupColumn("Outputs");
        ImGui::TableSetupColumn("Rate");
        ImGui::TableSetupColumn("Default");
        ImGui::TableHeadersRow();
        for ( const auto& device : graph.devices() ) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(device.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(flowName(device.flow));
            ImGui::TableNextColumn();
            ImGui::Text("%u", device.inputChannels);
            ImGui::TableNextColumn();
            ImGui::Text("%u", device.outputChannels);
            ImGui::TableNextColumn();
            ImGui::Text("%u", device.sampleRate);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(device.isDefault ? "Yes" : "");
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Create route");
    deviceCombo("Source", graph.devices(), Core::canCapture, m_sourceSelection);
    deviceCombo("Sink", graph.devices(), Core::canRender, m_sinkSelection);
    ImGui::SliderFloat("Gain", &m_newRouteGain, 0.0F, 4.0F, "%.2f");
    const auto canCreate = sourceCount > 0 && sinkCount > 0;
    ImGui::BeginDisabled(!canCreate);
    if ( feedbackButton("Add route") ) {
        const auto* source =
            nthDevice(graph.devices(), Core::canCapture, m_sourceSelection);
        const auto* sink =
            nthDevice(graph.devices(), Core::canRender, m_sinkSelection);
        auto result = graph.createRoute(source->id, sink->id, m_newRouteGain);
        if ( !result ) {
            m_error = Core::routingErrorMessage(result.error());
        } else {
            m_error.clear();
        }
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Routes");
    Core::RouteId removeId{};
    for ( const auto& route : graph.routes() ) {
        ImGui::PushID(static_cast<int>(route.id));
        const auto* source = graph.findDevice(route.sourceDeviceId);
        const auto* sink   = graph.findDevice(route.sinkDeviceId);
        ImGui::Text("%s -> %s",
                    source ? source->name.c_str() : "Disconnected",
                    sink ? sink->name.c_str() : "Disconnected");
        ImGui::SameLine();

        auto gain  = route.gain;
        auto muted = route.muted;
        ImGui::SetNextItemWidth(180.0F);
        const auto gainChanged =
            ImGui::SliderFloat("##RouteGain", &gain, 0.0F, 4.0F, "%.2f");
        ImGui::SameLine();
        const auto muteChanged = ImGui::Checkbox("Mute", &muted);
        ImGui::SameLine();
        if ( feedbackSmallButton("Remove") ) removeId = route.id;
        if ( gainChanged || muteChanged ) {
            const auto result = graph.updateRoute(route.id, gain, muted);
            if ( !result ) m_error = Core::routingErrorMessage(result.error());
        }
        ImGui::PopID();
    }
    if ( removeId != 0 ) static_cast<void>(graph.removeRoute(removeId));

    ImGui::End();
    return actions;
}

void MainView::setError(std::string message)
{
    m_error = std::move(message);
}

void MainView::normalizeSelections(const Core::RoutingGraph& graph) noexcept
{
    const auto sourceCount = countDevices(graph.devices(), Core::canCapture);
    const auto sinkCount   = countDevices(graph.devices(), Core::canRender);
    m_sourceSelection =
        sourceCount == 0
            ? 0
            : std::min(m_sourceSelection, static_cast<int>(sourceCount - 1));
    m_sinkSelection =
        sinkCount == 0
            ? 0
            : std::min(m_sinkSelection, static_cast<int>(sinkCount - 1));
}

}  // namespace AudioRoads::UI
