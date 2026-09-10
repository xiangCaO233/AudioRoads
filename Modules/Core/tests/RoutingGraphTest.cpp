#include "RoutingGraph.h"

#include <cassert>
#include <utility>

using AudioRoads::Core::ApplicationIdentity;
using AudioRoads::Core::AudioEndpointSnapshot;
using AudioRoads::Core::AudioSource;
using AudioRoads::Core::AudioSourceKind;
using AudioRoads::Core::AudioTarget;
using AudioRoads::Core::AudioTargetKind;
using AudioRoads::Core::RoutingError;
using AudioRoads::Core::RoutingGraph;

/// @brief 覆盖设备/应用来源、播放/麦克风目标及热插拔路由语义。
int main()
{
    RoutingGraph graph;
    // 测试快照同时覆盖两种来源和两种目标，确保图约束不依赖平台设备方向。
    AudioEndpointSnapshot endpoints{
        .sources =
            {
                AudioSource{ .id         = "device:microphone",
                             .name       = "Microphone",
                             .backend    = "Test",
                             .kind       = AudioSourceKind::DeviceInput,
                             .channels   = 2,
                             .sampleRate = 48'000,
                             .isDefault  = true,
                             .application = std::nullopt },
                AudioSource{
                    .id         = "application:player",
                    .name       = "Music Player",
                    .backend    = "Test",
                    .kind       = AudioSourceKind::ApplicationOutput,
                    .channels   = 2,
                    .sampleRate = 48'000,
                    .application = ApplicationIdentity{
                        .processIds = { 42 }, .stableId = "music-player" } },
            },
        .targets =
            {
                AudioTarget{ .id         = "device:speakers",
                             .name       = "Speakers",
                             .backend    = "Test",
                             .kind       = AudioTargetKind::DeviceOutput,
                             .channels   = 2,
                             .sampleRate = 48'000,
                             .isDefault  = true },
                AudioTarget{ .id         = "virtual:microphone",
                             .name       = "AudioRoads Microphone",
                             .backend    = "Test",
                             .kind       = AudioTargetKind::VirtualMicrophone,
                             .channels   = 2,
                             .sampleRate = 48'000 },
            },
    };
    graph.replaceEndpoints(std::move(endpoints));
    // replace 后快照所有权已进入 graph，后续断言只观察图内拥有型值。
    assert(graph.sources().size() == 2);
    assert(graph.targets().size() == 2);

    // 单个应用输出与物理录音输入使用完全相同的纯数据路由入口。
    const auto playback =
        graph.createRoute("application:player", "device:speakers", 0.75F);
    assert(playback.has_value());
    assert(*playback != 0);
    // 第一条路由从应用方块连接播放目标，并保留用户提交的线性增益。
    assert(graph.routes().front().gain == 0.75F);

    // 虚拟麦克风在图内是消费者，系统侧才将其呈现为新的录音设备。
    const auto microphone =
        graph.createRoute("device:microphone", "virtual:microphone", 0.5F);
    assert(microphone.has_value());
    assert(graph.routes().size() == 2);
    assert(graph.updateRoute(*microphone, 1.25F, true).has_value());
    assert(graph.routes().back().muted);
    // 参数更新不能改变端点 ID，否则会绕过创建时的存在性和重复边校验。
    assert(graph.routes().back().sourceId == "device:microphone");
    assert(graph.routes().back().targetId == "virtual:microphone");

    const auto duplicate =
        graph.createRoute("application:player", "device:speakers");
    // 重复定义按端点有序对判断，与增益是否不同无关。
    assert(!duplicate.has_value());
    assert(duplicate.error() == RoutingError::DuplicateRoute);
    // 失败不消耗 ID 或追加半成品，路由数量必须保持不变。
    assert(graph.routes().size() == 2);

    const auto missingTarget =
        graph.createRoute("application:player", "missing");
    assert(!missingTarget.has_value());
    assert(missingTarget.error() == RoutingError::TargetNotFound);
    // 来源仍在线不能掩盖目标缺失，错误必须精确指向连接的失败端。

    // 端点快照消失不删除用户图，稳定 ID 恢复后可由执行层重新接通。
    graph.replaceEndpoints({});
    // 空快照只表示所有端点暂时离线，不等价于用户要求清空项目拓扑。
    assert(graph.routes().size() == 2);
    // 离线路由仍可通过自身 RouteId 删除，操作不要求两端当前可发现。
    assert(graph.removeRoute(*playback));
    assert(graph.removeRoute(*microphone));
    assert(graph.routes().empty());
    // 删除完毕后图保持可复用，nextRouteId 不回退也不会与旧 UI 动作冲突。
    return 0;
}
