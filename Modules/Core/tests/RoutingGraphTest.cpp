#include "RoutingGraph.h"

#include <cassert>
#include <utility>
#include <vector>

using AudioRoads::Core::AudioDevice;
using AudioRoads::Core::DeviceFlow;
using AudioRoads::Core::RoutingError;
using AudioRoads::Core::RoutingGraph;

/// @brief 覆盖有效路由、重复路由、方向约束和设备热插拔语义。
int main()
{
    RoutingGraph graph;
    graph.replaceDevices(std::vector<AudioDevice>{
        { .id            = "microphone",
          .name          = "Microphone",
          .backend       = "Test",
          .flow          = DeviceFlow::Input,
          .inputChannels = 2,
          .sampleRate    = 48'000 },
        { .id             = "speakers",
          .name           = "Speakers",
          .backend        = "Test",
          .flow           = DeviceFlow::Output,
          .outputChannels = 2,
          .sampleRate     = 48'000 },
    });

    // 正常设备对创建后应获得非零、可更新的稳定 ID。
    const auto created = graph.createRoute("microphone", "speakers", 0.75F);
    assert(created.has_value());
    assert(*created != 0);
    assert(graph.routes().size() == 1);
    assert(graph.updateRoute(*created, 0.5F, true).has_value());
    assert(graph.routes().front().muted);

    // 重复设备对必须由图层拒绝，而不是交给平台后端制造双重流。
    const auto duplicate = graph.createRoute("microphone", "speakers");
    assert(!duplicate.has_value());
    assert(duplicate.error() == RoutingError::DuplicateRoute);

    const auto reversed = graph.createRoute("speakers", "microphone");
    assert(!reversed.has_value());
    assert(reversed.error() == RoutingError::SourceCannotCapture);

    // 热拔出只更新设备快照；路由配置保留，等待同 ID 设备恢复。
    graph.replaceDevices({});
    assert(graph.routes().size() == 1);
    assert(graph.removeRoute(*created));
    assert(graph.routes().empty());
    return 0;
}
