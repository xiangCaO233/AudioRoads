#include "MainView.h"

#include "AudioTypes.h"
#include "UiWidgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace AudioRoads::UI
{
namespace
{

/// @brief 返回来源类别的短标签，文本不参与持久化或业务判断。
[[nodiscard]] const char* sourceKindName(Core::AudioSourceKind kind) noexcept
{
    switch ( kind ) {
    case Core::AudioSourceKind::DeviceInput: return "Input device";
    case Core::AudioSourceKind::ApplicationOutput: return "Application";
    }
    return "Unknown";
}

/// @brief 返回路由目标类别的短标签。
[[nodiscard]] const char* targetKindName(Core::AudioTargetKind kind) noexcept
{
    switch ( kind ) {
    case Core::AudioTargetKind::DeviceOutput: return "Playback device";
    case Core::AudioTargetKind::VirtualMicrophone: return "Virtual microphone";
    }
    return "Unknown";
}

/// @brief 计算给定目标当前接收的路由数量。
/// @param graph 当前帧只读观察的控制面路由图。
/// @param targetId 目标稳定 ID，不要求目标此刻在线。
/// @return 包含离线路由在内的入边数量，用于表达配置中的混音关系。
[[nodiscard]] std::size_t incomingRouteCount(const Core::RoutingGraph& graph,
                                             std::string_view targetId) noexcept
{
    return static_cast<std::size_t>(std::ranges::count(
        graph.routes(), targetId, &Core::AudioRoute::targetId));
}

/// @brief 把节点局部坐标转换为当前可滚动画布的屏幕坐标。
/// @details ImDrawList 使用屏幕坐标，而布局算法使用以画布内容左上角为原点的
/// 局部坐标。滚动位置已经包含在 origin 中，转换处不能再次减去滚动量。
/// @return 可直接交给 ImDrawList 和 SetCursorScreenPos 的绝对位置。
[[nodiscard]] ImVec2 screenPoint(const ImVec2& origin, float x,
                                 float y) noexcept
{
    return { origin.x + x, origin.y + y };
}

/// @brief 绘制来源节点悬停时的完整身份，避免长稳定 ID 挤压节点正文。
/// @details tooltip 紧跟来源 InvisibleButton 调用，依赖当前 ImGui item 的悬停
/// 状态；不得延迟到绘制其他节点后执行。应用 PID 只用于诊断，不作为连线键。
void drawSourceTooltip(const Core::AudioSource& source)
{
    if ( !ImGui::BeginItemTooltip() ) return;
    ImGui::TextUnformatted(source.name.c_str());
    ImGui::Separator();
    ImGui::Text("Backend: %s", source.backend.c_str());
    ImGui::TextWrapped("ID: %s", source.id.c_str());
    if ( source.application ) {
        ImGui::TextWrapped("Application: %s",
                           source.application->stableId.c_str());
        for ( const auto processId : source.application->processIds ) {
            ImGui::Text("PID: %llu",
                        static_cast<unsigned long long>(processId));
        }
    }
    ImGui::EndTooltip();
}

/// @brief 绘制端点节点及已有路由，并处理“先来源、后目标”的连线操作。
///
/// 节点布局在每帧由稳定快照顺序推导，不缓存 ImGui 地址。多条边汇入同一目标
/// 即表示该目标的混音输入；实际音频回调仍由 Audio 层在帧外建立。
/// @param graph 当前帧允许编辑的纯内存路由图。
/// @param pendingSourceId 已选来源的稳定 ID，连接成功或端点离线时清空。
/// @param newRouteGain 新连接提交给 Core 校验的线性增益草稿。
/// @param error 操作失败时写入的跨帧用户提示，成功时清空。
void drawRoutingCanvas(Core::RoutingGraph& graph, std::string& pendingSourceId,
                       float newRouteGain, std::string& error)
{
    constexpr float NODE_WIDTH  = 250.0F;
    constexpr float NODE_HEIGHT = 92.0F;
    constexpr float ROW_STEP    = 118.0F;
    constexpr float SIDE_PAD    = 36.0F;
    constexpr float TOP_PAD     = 34.0F;

    // 子窗口自身负责纵向和横向裁剪；节点数量增加时只扩展内容高度，不扩大主
    // 工作区。最小宽度让来源和目标之间始终留有可辨识的连线空间。
    ImGui::BeginChild("RoutingCanvas",
                      ImVec2{ 0.0F, 440.0F },
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    // origin 已包含当前滚动偏移，后续所有绘制和 item 命中区域必须共用该基准。
    const auto origin = ImGui::GetCursorScreenPos();
    // 两列共用行步长但彼此数量独立，内容高度必须取较大列，避免末尾节点被裁掉。
    const auto rows  = std::max(graph.sources().size(), graph.targets().size());
    const auto width = std::max(ImGui::GetContentRegionAvail().x, 900.0F);
    const auto height =
        std::max(410.0F, TOP_PAD * 2.0F + ROW_STEP * static_cast<float>(rows));
    const ImVec2 canvasSize{ width, height };
    // Dummy 先声明完整可滚动范围；随后节点 item 会暂时把光标移回各自矩形。
    ImGui::Dummy(canvasSize);

    auto* drawList = ImGui::GetWindowDrawList();
    // drawList 由当前 child window 拥有，因此自动继承其裁剪矩形。
    const auto targetX = width - SIDE_PAD - NODE_WIDTH;
    // 来源输出端口固定在卡片右侧中心，同一索引算法同时服务边锚点和圆点绘制。
    // lambda 只捕获本帧标量，不逃逸到 ImGui 帧外。
    const auto outputPoint = [&](std::size_t index) {
        return screenPoint(origin,
                           SIDE_PAD + NODE_WIDTH,
                           TOP_PAD + ROW_STEP * static_cast<float>(index) +
                               NODE_HEIGHT * 0.5F);
    };
    // 目标输入端口与来源端口相向布置，目标列随画布宽度移动以利用宽屏空间。
    // 两端使用相同纵向公式，同行连接保持水平且跨行连接自然弯曲。
    const auto inputPoint = [&](std::size_t index) {
        return screenPoint(origin,
                           targetX,
                           TOP_PAD + ROW_STEP * static_cast<float>(index) +
                               NODE_HEIGHT * 0.5F);
    };

    // 边先于节点绘制，使端口圆点和节点边框始终覆盖连接线端部。路由只保存
    // 稳定 ID，因此这里必须在当前快照中重新解析两端，不能缓存旧迭代器。
    for ( const auto& route : graph.routes() ) {
        const auto source = std::ranges::find(
            graph.sources(), route.sourceId, &Core::AudioSource::id);
        const auto target = std::ranges::find(
            graph.targets(), route.targetId, &Core::AudioTarget::id);
        if ( source == graph.sources().end() ||
             target == graph.targets().end() ) {
            // 离线端点的路由仍保存在模型中，但画布没有可锚定节点时不伪造位置。
            continue;
        }
        const auto sourceIndex =
            static_cast<std::size_t>(source - graph.sources().begin());
        const auto targetIndex =
            static_cast<std::size_t>(target - graph.targets().begin());
        const auto from = outputPoint(sourceIndex);
        const auto to   = inputPoint(targetIndex);
        // 控制点只沿 X 轴偏移，保证信号方向从左到右；窄布局仍保留最小曲率。
        const auto bend = std::max(90.0F, (to.x - from.x) * 0.42F);
        // 静音路由仍可见但降低颜色和线宽，用户可以区分“存在但无贡献”和断线。
        const auto color = route.muted ? IM_COL32(110, 116, 128, 170)
                                       : IM_COL32(72, 178, 255, 230);
        drawList->AddBezierCubic(from,
                                 ImVec2{ from.x + bend, from.y },
                                 ImVec2{ to.x - bend, to.y },
                                 to,
                                 color,
                                 route.muted ? 2.0F : 3.0F);
    }

    // 来源节点只有输出端口。点击整张卡片等价于拿起一条待连接线，减少必须精确
    // 点中小圆点的操作成本；再次点击同一来源则取消选择。
    for ( std::size_t index = 0; index < graph.sources().size(); ++index ) {
        const auto& source  = graph.sources()[index];
        const auto  topLeft = screenPoint(
            origin, SIDE_PAD, TOP_PAD + ROW_STEP * static_cast<float>(index));
        const ImVec2 bottomRight{ topLeft.x + NODE_WIDTH,
                                  topLeft.y + NODE_HEIGHT };
        const auto   selected = source.id == pendingSourceId;
        // 选择态同时改变填充和描边，即使色觉难以区分也能从边框粗细识别。
        const auto fill =
            selected ? IM_COL32(45, 91, 130, 255) : IM_COL32(36, 43, 54, 255);
        drawList->AddRectFilled(topLeft, bottomRight, fill, 8.0F);
        drawList->AddRect(topLeft,
                          bottomRight,
                          selected ? IM_COL32(89, 196, 255, 255)
                                   : IM_COL32(91, 104, 124, 255),
                          8.0F,
                          0,
                          selected ? 3.0F : 1.5F);
        drawList->AddText(nullptr,
                          0.0F,
                          screenPoint(topLeft, 12.0F, 10.0F),
                          IM_COL32_WHITE,
                          source.name.c_str(),
                          nullptr,
                          NODE_WIDTH - 24.0F);
        // 格式信息来自枚举快照，零表示平台未报告；真正开流时仍需重新协商。
        const auto detail = std::string{ sourceKindName(source.kind) } + "  " +
                            std::to_string(source.channels) + " ch / " +
                            std::to_string(source.sampleRate) + " Hz";
        drawList->AddText(screenPoint(topLeft, 12.0F, 58.0F),
                          IM_COL32(174, 186, 203, 255),
                          detail.c_str());
        drawList->AddCircleFilled(
            outputPoint(index), 7.0F, IM_COL32(89, 196, 255, 255));

        ImGui::SetCursorScreenPos(topLeft);
        ImGui::PushID(source.id.c_str());
        // 稳定 ID 进入 ImGui ID 栈，显示名重复或运行中改变都不会串用交互状态。
        if ( ImGui::InvisibleButton("SourceNode",
                                    ImVec2{ NODE_WIDTH, NODE_HEIGHT }) ) {
            // 再次点击已选来源可取消待连接状态，防止误连到稍后点击的目标。
            pendingSourceId = selected ? std::string{} : source.id;
        }
        drawSourceTooltip(source);
        ImGui::PopID();
    }

    // 目标节点只有输入端口。多个来源可以依次连接同一目标，路由图的重复边约束
    // 只禁止完全相同的来源/目标对，不限制合法的多路混音。
    for ( std::size_t index = 0; index < graph.targets().size(); ++index ) {
        const auto& target  = graph.targets()[index];
        const auto  topLeft = screenPoint(
            origin, targetX, TOP_PAD + ROW_STEP * static_cast<float>(index));
        const ImVec2 bottomRight{ topLeft.x + NODE_WIDTH,
                                  topLeft.y + NODE_HEIGHT };
        const auto   canConnect = !pendingSourceId.empty();
        // 有待连接来源时目标变绿，明确提示下一次点击会修改路由图而非选择目标。
        drawList->AddRectFilled(
            topLeft,
            bottomRight,
            canConnect ? IM_COL32(54, 68, 50, 255) : IM_COL32(36, 43, 54, 255),
            8.0F);
        drawList->AddRect(topLeft,
                          bottomRight,
                          canConnect ? IM_COL32(128, 211, 112, 255)
                                     : IM_COL32(91, 104, 124, 255),
                          8.0F,
                          0,
                          canConnect ? 2.5F : 1.5F);
        drawList->AddText(nullptr,
                          0.0F,
                          screenPoint(topLeft, 12.0F, 10.0F),
                          IM_COL32_WHITE,
                          target.name.c_str(),
                          nullptr,
                          NODE_WIDTH - 24.0F);
        // 入边计数直接说明目标承担混音汇聚；无需额外创建只作展示的 mixer 对象。
        const auto routeCount = incomingRouteCount(graph, target.id);
        const auto detail     = std::string{ targetKindName(target.kind) } +
                            "  Mix " + std::to_string(routeCount) + " inputs";
        drawList->AddText(screenPoint(topLeft, 12.0F, 58.0F),
                          IM_COL32(174, 186, 203, 255),
                          detail.c_str());
        drawList->AddCircleFilled(
            inputPoint(index), 7.0F, IM_COL32(128, 211, 112, 255));

        ImGui::SetCursorScreenPos(topLeft);
        ImGui::PushID(target.id.c_str());
        // 无待连接来源时目标仍保留 tooltip，但点击不产生任何业务状态变化。
        if ( ImGui::InvisibleButton("TargetNode",
                                    ImVec2{ NODE_WIDTH, NODE_HEIGHT }) &&
             canConnect ) {
            // Core 再次验证端点与重复边；成功后清空来源，避免连续误建相同路由。
            auto result =
                graph.createRoute(pendingSourceId, target.id, newRouteGain);
            if ( result ) {
                pendingSourceId.clear();
                error.clear();
            } else {
                error = Core::routingErrorMessage(result.error());
            }
        }
        // 目标 tooltip 紧随当前 InvisibleButton，展示完整 ID
        // 和枚举格式而不挤占卡片。
        if ( ImGui::BeginItemTooltip() ) {
            ImGui::TextUnformatted(target.name.c_str());
            ImGui::Separator();
            ImGui::Text("Backend: %s", target.backend.c_str());
            ImGui::TextWrapped("ID: %s", target.id.c_str());
            ImGui::Text(
                "Format: %u ch / %u Hz", target.channels, target.sampleRate);
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }

    // 恢复到画布末尾，避免最后一个节点的回溯光标缩小滚动内容范围。
    ImGui::SetCursorScreenPos(screenPoint(origin, canvasSize.x, canvasSize.y));
    ImGui::EndChild();
}

}  // namespace

MainViewActions MainView::draw(Core::RoutingGraph& graph,
                               const char*         backendName)
{
    // 绘制路径只编辑内存图；应用捕获、设备创建和流启动必须作为帧外动作执行。
    MainViewActions actions;
    normalizePendingConnection(graph);

    // WorkPos/WorkSize 排除系统任务栏或菜单区域；主工作区不持久化旧显示器坐标。
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
    // 刷新只产生动作值，Application 在帧提交后访问同步平台枚举接口。
    if ( feedbackButton("Refresh endpoints") ) actions.refreshEndpoints = true;

    if ( !m_error.empty() ) {
        // 平台枚举和路由校验共用一处错误区，保留到下一次成功操作或显式覆盖。
        // push/pop 在同一分支配平，错误颜色不会泄漏到节点或线路控制项。
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0F, 0.4F, 0.35F, 1.0F });
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::SeparatorText("Routing canvas");
    ImGui::TextDisabled(
        "Click a source block, then a target block. Multiple incoming links "
        "are mixed.");
    // 增益是下一条连接的草稿；现有线路在下方面板中各自独立编辑。
    ImGui::SliderFloat("Gain", &m_newRouteGain, 0.0F, 4.0F, "%.2f");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_pendingSourceId.empty());
    if ( feedbackButton("Cancel connection") ) {
        // 取消只清理 UI 草稿，不删除已存在路由或改变当前新路由增益。
        m_pendingSourceId.clear();
    }
    ImGui::EndDisabled();
    drawRoutingCanvas(graph, m_pendingSourceId, m_newRouteGain, m_error);

    ImGui::SeparatorText("Routes");
    // 画布负责拓扑总览，列表负责精确数值编辑；两者共享同一 RoutingGraph，
    // 不建立可能与真实路由状态漂移的 UI 副本。
    // 删除延迟到遍历后，避免 range-for 中修改 routes 使迭代器失效。
    Core::RouteId removeId{};
    for ( const auto& route : graph.routes() ) {
        // RouteId 进入 ImGui ID 栈，使多条线路复用相同控件标签而不共享状态。
        ImGui::PushID(static_cast<int>(route.id));
        const auto* source = graph.findSource(route.sourceId);
        const auto* target = graph.findTarget(route.targetId);
        // 离线端点继续展示断开占位，符合 RoutingGraph 保留热拔插配置的契约。
        ImGui::Text("%s -> %s",
                    source ? source->name.c_str() : "Disconnected source",
                    target ? target->name.c_str() : "Disconnected target");
        ImGui::SameLine();

        auto gain  = route.gain;
        auto muted = route.muted;
        // 局部草稿避免 ImGui 在 Core 校验前直接写入 route 容器元素。
        ImGui::SetNextItemWidth(180.0F);
        const auto gainChanged =
            ImGui::SliderFloat("##RouteGain", &gain, 0.0F, 4.0F, "%.2f");
        ImGui::SameLine();
        const auto muteChanged = ImGui::Checkbox("Mute", &muted);
        ImGui::SameLine();
        if ( feedbackSmallButton("Remove") ) removeId = route.id;
        if ( gainChanged || muteChanged ) {
            // updateRoute 集中校验增益并按稳定 ID 更新，UI 不复制业务约束。
            const auto result = graph.updateRoute(route.id, gain, muted);
            if ( !result ) m_error = Core::routingErrorMessage(result.error());
        }
        ImGui::PopID();
    }
    // 删除动作延迟到 range-for 结束，保证本帧所有迭代器和 route 引用有效。
    if ( removeId != 0 ) static_cast<void>(graph.removeRoute(removeId));

    ImGui::End();
    return actions;
}

void MainView::setError(std::string message)
{
    // 值传递允许调用者移动临时错误；视图拥有文本并可跨多个显示帧安全展示。
    m_error = std::move(message);
}

void MainView::normalizePendingConnection(const Core::RoutingGraph& graph)
{
    // 只在存在待连接 ID 时查询，普通帧不做无意义的线性来源搜索。
    if ( !m_pendingSourceId.empty() &&
         graph.findSource(m_pendingSourceId) == nullptr ) {
        // 来源热拔插后不能把旧选择误套到新快照中的相邻节点。
        m_pendingSourceId.clear();
    }
}

}  // namespace AudioRoads::UI
