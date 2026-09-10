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
    // 人工稳定 ID 隔离平台后端，并明确输入、输出能力来自通道方向。
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
    // 测试数据不打开真实设备，使控制面不变量在三平台和无音频服务 CI 中确定。

    // 正常设备对创建后应获得非零、可更新的稳定 ID。
    const auto created = graph.createRoute("microphone", "speakers", 0.75F);
    assert(created.has_value());
    assert(*created != 0);
    // 零由 UI 用作“无删除请求”哨兵，图层不得向真实路由发放该值。
    assert(graph.routes().size() == 1);
    // 增益和静音属于同一路由的一次控制面更新，端点 ID 保持不变。
    assert(graph.updateRoute(*created, 0.5F, true).has_value());
    assert(graph.routes().front().muted);
    // 更新不能替换端点，后续重复检查仍应命中原有的有向设备对。

    // 重复设备对必须由图层拒绝，而不是交给平台后端制造双重流。
    const auto duplicate = graph.createRoute("microphone", "speakers");
    // 重复失败不得插入第二项或消耗一个对外可见的路由对象。
    assert(!duplicate.has_value());
    assert(duplicate.error() == RoutingError::DuplicateRoute);

    // 反向端点不是另一条合法路由：输出设备不能作为采集源。
    const auto reversed = graph.createRoute("speakers", "microphone");
    assert(!reversed.has_value());
    // 具体方向错误比笼统“端点不存在”更能证明图层读取了设备能力。
    assert(reversed.error() == RoutingError::SourceCannotCapture);

    // 热拔出只更新设备快照；路由配置保留，等待同 ID 设备恢复。
    graph.replaceDevices({});
    // 不缓存 findDevice 返回指针跨越该替换；只通过稳定 ID 验证配置仍存在。
    assert(graph.routes().size() == 1);
    // 显式删除返回 true 且容器同步变空，区分热拔出与用户删除两种状态转折。
    assert(graph.removeRoute(*created));
    assert(graph.routes().empty());
    // 容器为空证明删除修改了图，而不只是返回了成功标志。
    return 0;
}
