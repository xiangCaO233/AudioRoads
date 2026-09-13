#pragma once

#include "IAudioBackend.h"

#include <expected>
#include <memory>
#include <string_view>

namespace AudioRoads::Audio
{

/// @brief 路由图中唯一虚拟麦克风目标的稳定 ID。
/// @details 该 ID 不包含 PipeWire object.serial，因此刷新节点或服务重连不会让
/// 已保存的路由失去目标身份。平台执行层会把它解析到下方内部接收节点。
inline constexpr std::string_view PIPEWIRE_VIRTUAL_MICROPHONE_TARGET_ID =
    "pipewire:virtual-microphone:default";

/// @brief 其他应用作为录音设备打开的公开 PipeWire Source 节点名。
/// @details 枚举器用精确节点名识别自有 Source，防止它被误列为可回送来源。
inline constexpr std::string_view PIPEWIRE_VIRTUAL_MICROPHONE_SOURCE_NODE =
    "audioroads.virtual-microphone";

/// @brief 只供 AudioRoads playback 流写入的内部 PipeWire 输入流节点名。
/// @details 它使用 Stream/Input/Audio 而非 Audio/Sink，桌面应用不会把内部注入点
/// 当作普通扬声器；明确关闭自动连接后也不会自行采集系统默认麦克风。
inline constexpr std::string_view PIPEWIRE_VIRTUAL_MICROPHONE_INPUT_NODE =
    "audioroads.virtual-microphone.input";

/// @brief 在当前 PipeWire 会话中托管常驻虚拟麦克风节点对。
/// @details loopback 的内部输入流接收路由引擎混音，公开 Audio/Source 将同一 PCM
/// 暴露给浏览器、通讯软件和录音程序。节点寿命覆盖后端，编辑路由只会重建送入
/// 内部流的 playback，不会销毁其他应用正在打开的麦克风端点。
class PipeWireVirtualMicrophone final
{
public:
    /// @brief 构造尚未接触 PipeWire 服务的空所有者。
    PipeWireVirtualMicrophone();

    /// @brief 停止模块循环并按依赖逆序释放所有原生对象。
    ~PipeWireVirtualMicrophone();

    PipeWireVirtualMicrophone(const PipeWireVirtualMicrophone&) = delete;
    PipeWireVirtualMicrophone& operator=(const PipeWireVirtualMicrophone&) =
        delete;
    PipeWireVirtualMicrophone(PipeWireVirtualMicrophone&&)            = delete;
    PipeWireVirtualMicrophone& operator=(PipeWireVirtualMicrophone&&) = delete;

    /// @brief 确保公开 Source 与内部输入流已经提交到默认 PipeWire 会话。
    /// @return 已运行时幂等成功；创建 loop、context
    /// 或模块失败时返回控制面错误。
    /// @warning 可能启动线程并加载模块，只能从后端枚举等低频控制路径调用。
    [[nodiscard]] std::expected<void, AudioBackendError> ensurePublished();

private:
    /// @brief 隔离 impl-module、main-loop、listener 与线程类型。
    struct Implementation;

    /// @brief 固定地址覆盖模块 listener 的全部可能回调。
    std::unique_ptr<Implementation> m_implementation;
};

}  // namespace AudioRoads::Audio
