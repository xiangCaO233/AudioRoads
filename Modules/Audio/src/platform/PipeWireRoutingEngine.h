#pragma once

#include "IAudioBackend.h"

#include <expected>
#include <memory>
#include <span>
#include <string>

namespace AudioRoads::Audio
{

/// @brief 枚举阶段记录的聚合来源到原生 PipeWire 节点映射。
/// @details 一个应用来源可对应多个短生命周期节点；稳定 sourceId 留在 Core，
/// targetObject 只在 Linux Audio 层用于建立当前流。
/// 映射只属于最近一次成功的完整 registry 快照。刷新失败时后端继续保留旧映射，
/// 与 AudioService 保留旧端点列表的事务语义一致；成功刷新则整体替换，不能把两轮
/// 节点 serial 合并，否则已经退出的应用流可能被再次尝试连接。
struct PipeWireSourceBinding {
    /// @brief 与 RoutingGraph 中来源一致的稳定标识。
    /// @details 应用来源通常以 process binary 或 application name
    /// 聚合，不直接等于 任一 PipeWire node.name，因此必须作为显式关联键保存。
    std::string sourceId;

    /// @brief 可写入 PipeWire `target.object` 的 object.serial 字符串。
    /// @details serial 只保证当前服务实例内可重新解析；它不会进入持久化 Core
    /// DTO， 下一轮 registry 快照必须重新取得，不能跨 PipeWire 服务重启复用。
    std::string targetObject;
};

/// @brief 将纯路由图编译为 PipeWire 实时采集、缓冲和播放流。
/// @details 实现隐藏全部 PipeWire 类型，头文件不会把平台 SDK 传播给其他模块。
///
/// 一个稳定来源只建立一个采集流并把样本扇出到每条关联边；每条边拥有独立 SPSC，
/// 因此来源一对多时仍满足单消费者约束。一个目标只建立一个播放流，在该回调中读取
/// 全部入边、应用各自增益/静音并调用 Core 混音器，避免依赖系统 mixer
/// 表达路由语义。
///
/// 引擎固定以交错 float32、48 kHz、双声道与 PipeWire 协商，让平台 adapter 完成
/// 当前设备的基本格式和通道转换。本阶段的环形缓冲吸收短期调度抖动；不同硬件时钟
/// 长期漂移所需的占用量反馈重采样尚未放进 Core，不能用阻塞等待代替。
///
/// 所有拓扑分配、字符串处理和流创建均发生在 synchronize 的控制路径。PipeWire
/// process 回调只访问稳定指针、预分配样本区和无锁标量；析构与重建会先同步停掉
/// 回调，之后才释放它们借用的状态。
class PipeWireRoutingEngine final
{
public:
    /// @brief 构造尚未连接服务的空控制对象，首条在线路由到来时才启动线程。
    PipeWireRoutingEngine();

    /// @brief 停止所有实时回调并释放 PipeWire 流及其预分配工作区。
    ~PipeWireRoutingEngine();

    PipeWireRoutingEngine(const PipeWireRoutingEngine&)            = delete;
    PipeWireRoutingEngine& operator=(const PipeWireRoutingEngine&) = delete;
    PipeWireRoutingEngine(PipeWireRoutingEngine&&)                 = delete;
    PipeWireRoutingEngine& operator=(PipeWireRoutingEngine&&)      = delete;

    /// @brief 同步在线路由；拓扑变化重建流，仅参数变化原子更新。
    /// @param graph 当前控制面快照，函数返回后实现不保留其引用。
    /// @param sourceBindings 本轮枚举得到的聚合应用来源原生节点。
    /// @return 所有在线流完成格式协商时成功；同步创建错误、异步流错误和超时均
    /// 返回可展示值。离线端点的路由不是错误，它们保留在 Core 并等待刷新恢复。
    /// @warning 只能从 UI 帧外控制线程调用；不得从任何 PipeWire process
    /// 回调调用。
    [[nodiscard]] std::expected<void, AudioBackendError> synchronize(
        const Core::RoutingGraph&              graph,
        std::span<const PipeWireSourceBinding> sourceBindings);

private:
    /// @brief 隔离 PipeWire 句柄、回调状态和预分配 PCM 工作区。
    /// @details unique_ptr 使公共类析构位置可控，同时避免平台头进入接口使用者。
    struct Implementation;

    /// @brief 生命周期覆盖全部平台回调；对象本身不可移动，地址始终稳定。
    std::unique_ptr<Implementation> m_implementation;
};

}  // namespace AudioRoads::Audio
