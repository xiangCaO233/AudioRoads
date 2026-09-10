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
///
/// 文本只用于当前 UI 展示，不作为持久化格式。switch 显式覆盖所有枚举值，
/// 最后降级保护来自损坏配置或未来未同步枚举的情况。
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
///
/// 直接复用 Core::canCapture/canRender，避免 UI 复制双工设备规则；函数每帧
/// 调用，因此只遍历借用快照，不建立过滤 vector。
[[nodiscard]] std::size_t countDevices(
    const std::vector<Core::AudioDevice>& devices, DevicePredicate predicate)
{
    // 函数指针限制 predicate 为无捕获、noexcept 的共享能力判断，避免帧内状态。
    return static_cast<std::size_t>(std::ranges::count_if(devices, predicate));
}

/// @brief 在筛选视图中查找第 index 个设备，返回当前快照的观察指针。
///
/// index 是筛选后的可见序号，不是原 vector 下标。负值和越界均自然返回空，
/// 让设备热插拔后的旧 UI 状态可被 normalizeSelections 恢复。
[[nodiscard]] const Core::AudioDevice* nthDevice(
    const std::vector<Core::AudioDevice>& devices, DevicePredicate predicate,
    int index) noexcept
{
    // 返回值仅观察 graph 的当前快照；调用者不得保存到下一帧或设备刷新之后。
    for ( const auto& device : devices ) {
        if ( !predicate(device) ) continue;
        if ( index-- == 0 ) return &device;
    }
    return nullptr;
}

/// @brief 绘制设备选择组合框，列表为空时显示明确占位文本。
///
/// 组合框只保存筛选索引，不保存设备地址或 label 字符串。所有 BeginCombo 成功
/// 路径都在同一帧调用 EndCombo，保持 ImGui 栈平衡。
void deviceCombo(const char*                           label,
                 const std::vector<Core::AudioDevice>& devices,
                 DevicePredicate predicate, int& selection)
{
    const auto* selectedDevice = nthDevice(devices, predicate, selection);
    // 筛选集合为空时保持可绘制的占位文本，避免解引用无效选择。
    const auto* preview = selectedDevice == nullptr
                              ? "No compatible device"
                              : selectedDevice->name.c_str();
    // preview 借用当前设备快照，仅传给本次即时 ImGui 调用，不跨帧缓存。
    if ( ImGui::BeginCombo(label, preview) ) {
        int index{};
        for ( const auto& device : devices ) {
            if ( !predicate(device) ) continue;
            // selected 使用筛选索引；原始 vector 中不兼容的端点不参与递增。
            const bool selected = index == selection;
            if ( ImGui::Selectable(device.name.c_str(), selected) ) {
                // 只提交整数草稿；设备地址在组合框关闭后无需继续有效。
                selection = index;
            }
            if ( selected ) ImGui::SetItemDefaultFocus();
            // 默认焦点只改善键盘导航，不改变当前业务选择或设备快照。
            ++index;
        }
        ImGui::EndCombo();
    }
}

}  // namespace

MainViewActions MainView::draw(Core::RoutingGraph& graph,
                               const char*         backendName)
{
    // 本函数处于逐帧 UI 路径，只允许读取设备快照并修改内存路由模型；平台查询、
    // 文件系统和阻塞等待必须通过返回的动作交给 Application。
    MainViewActions actions;
    // 热插拔会改变筛选后数量，每帧先收敛索引再取得观察指针。
    normalizeSelections(graph);

    const auto sourceCount = countDevices(graph.devices(), Core::canCapture);
    const auto sinkCount   = countDevices(graph.devices(), Core::canRender);
    // 计数仅用于交互可用性；真正提交仍由 RoutingGraph 重验全部业务约束。

    // WorkPos/WorkSize 排除系统菜单区域；无装饰窗口作为固定应用工作区。
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos);
    ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize);
    constexpr auto windowFlags = ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings;
    // 不恢复旧 ini 位置，防止显示器布局变化后工作区落到当前 viewport 之外。
    ImGui::Begin("AudioRoads Workspace###MainWorkspace", nullptr, windowFlags);
    // ### 后的稳定 ID 与可见标题解耦，未来本地化标题不会重置窗口内部状态。

    ImGui::TextUnformatted("AudioRoads");
    ImGui::SameLine();
    ImGui::TextDisabled("Native backend: %s", backendName);
    // 后端名称只用于诊断显示，不能反向驱动平台分支或路由业务规则。
    ImGui::SameLine();
    // 控件只产生值动作，平台枚举由 Application 在本帧提交后执行。
    if ( feedbackButton("Refresh devices") ) actions.refreshDevices = true;

    if ( !m_error.empty() ) {
        // 错误属于持久视图状态，显式 push/pop 颜色，不能污染后续正常控件。
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0F, 0.4F, 0.35F, 1.0F });
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::SeparatorText("Devices");
    if ( ImGui::BeginTable("DevicesTable",
                           6,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_Resizable) ) {
        // BeginTable 可能因裁剪返回 false，只有成功路径才能提交列和调用
        // EndTable。
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Flow");
        ImGui::TableSetupColumn("Inputs");
        ImGui::TableSetupColumn("Outputs");
        ImGui::TableSetupColumn("Rate");
        ImGui::TableSetupColumn("Default");
        ImGui::TableHeadersRow();
        for ( const auto& device : graph.devices() ) {
            // 表格只投影快照，不把格式化名称或默认标签写回 Core 数据。
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
    // 两个组合框分别使用 Core 能力谓词，双工设备可合法同时出现在两侧。
    deviceCombo("Source", graph.devices(), Core::canCapture, m_sourceSelection);
    deviceCombo("Sink", graph.devices(), Core::canRender, m_sinkSelection);
    ImGui::SliderFloat("Gain", &m_newRouteGain, 0.0F, 4.0F, "%.2f");
    // 草稿范围与 Core 契约一致，但 Core 仍防御 NaN 和非 UI 调用入口。
    const auto canCreate = sourceCount > 0 && sinkCount > 0;
    // BeginDisabled/EndDisabled 必须配对，避免禁用状态泄漏到已有路由控件。
    ImGui::BeginDisabled(!canCreate);
    if ( feedbackButton("Add route") ) {
        // 控件禁用只是交互提示；Core 仍对端点方向、重复项和增益二次校验。
        const auto* source =
            nthDevice(graph.devices(), Core::canCapture, m_sourceSelection);
        const auto* sink =
            nthDevice(graph.devices(), Core::canRender, m_sinkSelection);
        // canCreate 与开头规范化保证观察指针有效；Core 仍负责最终约束校验。
        auto result = graph.createRoute(source->id, sink->id, m_newRouteGain);
        if ( !result ) {
            // Core 枚举映射为用户可读错误，UI 不推断失败原因或修改半成品状态。
            m_error = Core::routingErrorMessage(result.error());
        } else {
            // 只有实际创建成功才清除历史错误；按钮激活本身不代表业务成功。
            m_error.clear();
        }
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Routes");
    // 不在 range-for 中修改 routes；先记录稳定 ID，遍历完成后再删除。
    Core::RouteId removeId{};
    for ( const auto& route : graph.routes() ) {
        // 同一行控件标签重复，RouteId 进入 ImGui ID 栈以隔离持久交互状态。
        ImGui::PushID(static_cast<int>(route.id));
        const auto* source = graph.findDevice(route.sourceDeviceId);
        const auto* sink   = graph.findDevice(route.sinkDeviceId);
        // 路由在热拔出后仍保留，缺失端点显示断开而不是隐式删除用户配置。
        ImGui::Text("%s -> %s",
                    source ? source->name.c_str() : "Disconnected",
                    sink ? sink->name.c_str() : "Disconnected");
        ImGui::SameLine();

        auto gain  = route.gain;
        auto muted = route.muted;
        // 编辑使用帧内副本，只有值变化时才提交，避免逐帧触发模型写操作。
        ImGui::SetNextItemWidth(180.0F);
        const auto gainChanged =
            ImGui::SliderFloat("##RouteGain", &gain, 0.0F, 4.0F, "%.2f");
        ImGui::SameLine();
        const auto muteChanged = ImGui::Checkbox("Mute", &muted);
        ImGui::SameLine();
        if ( feedbackSmallButton("Remove") ) removeId = route.id;
        // 单帧只延迟一个稳定 ID，避免保存 vector 迭代器或元素地址。
        if ( gainChanged || muteChanged ) {
            const auto result = graph.updateRoute(route.id, gain, muted);
            // 正常 UI 范围应通过校验，但图层错误仍必须可见，便于发现状态竞态。
            if ( !result ) m_error = Core::routingErrorMessage(result.error());
        }
        ImGui::PopID();
        // 每次 PushID 在同一路由迭代内配平，后续行不会继承前一项标识。
    }
    // RouteId 从 1 开始，零可作“本帧未请求删除”的无歧义哨兵。
    if ( removeId != 0 ) static_cast<void>(graph.removeRoute(removeId));

    // 顶层 Begin/End 在所有分支后统一配对，返回动作不携带 graph 观察指针。
    ImGui::End();
    return actions;
}

void MainView::setError(std::string message)
{
    // 按值接收后移动，调用方可传临时组合文本，视图独占其跨帧生命周期。
    m_error = std::move(message);
}

void MainView::normalizeSelections(const Core::RoutingGraph& graph) noexcept
{
    const auto sourceCount = countDevices(graph.devices(), Core::canCapture);
    const auto sinkCount   = countDevices(graph.devices(), Core::canRender);
    // 两个方向独立收敛，双工集合变化不会错误复用另一组合框的数量。
    // 空列表回到零以便后续设备出现；非空列表则夹到最后一个筛选后索引。
    // 这里不调用 nthDevice，避免用临时指针表达纯整数状态修复。
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
