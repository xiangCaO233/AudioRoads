#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AudioRoads::Core
{

/// @brief 路由引擎可读取的音频来源类型。
///
/// 枚举按数据进入 AudioRoads 的方向命名，不照搬操作系统控制面中的端点术语。
/// 新来源类型必须能够产出同一 PCM 契约，不能要求 Core 持有平台对象。
enum class AudioSourceKind : std::uint8_t {
    /// 物理或系统虚拟录音端点。
    DeviceInput,
    /// 单个桌面应用向操作系统提交的播放数据。
    ApplicationOutput,
};

/// @brief 路由混音结果可以写入的目标类型。
///
/// 两种目标都主动消费 Core 输出；VirtualMicrophone 仅在系统外部看来是输入
/// 设备。该方向约定避免把“麦克风”名称误当成图中的 source。
enum class AudioTargetKind : std::uint8_t {
    /// 由系统音频栈播放到扬声器、耳机或其他输出端点。
    DeviceOutput,
    /// 消费路由数据，并作为新录音设备向其他应用提供数据。
    VirtualMicrophone,
};

/// @brief 应用音频来源对应的稳定应用身份。
///
/// processIds 只标识本次运行实例，stableId 用可执行文件、包标识或平台应用 ID
/// 表达跨进程重启选择。一个桌面软件可能拥有多个音频进程，后端打开来源时应
/// 捕获当前稳定标识下的全部进程，而不是任意选择其中一个。
/// 枚举快照提交后进程可能退出，因此 processIds 只能作为开流候选；Audio 层
/// 必须重新解析 stableId 并容忍候选失效。Core 不解释任何平台应用标识格式。
struct ApplicationIdentity {
    /// @brief 当前应用的活跃音频进程集合；平台未提供时为空。
    std::vector<std::uint64_t> processIds;

    /// @brief 平台作用域内的稳定应用标识，不得包含原生对象地址。
    std::string stableId;
};

/// @brief 可由路由引擎采集的拥有型来源快照。
///
/// 来源可能是录音设备，也可能是某个应用的播放输出。它只保存稳定 ID、展示
/// 元数据和枚举时格式，不持有平台流或回调；真正打开时必须由后端重新解析。
/// kind 决定 Audio 层采用设备采集还是应用捕获，不改变路由图的数据方向。
/// channels/sampleRate 只服务于预览和协商提示，零明确表示枚举阶段未知。
struct AudioSource {
    /// @brief 后端作用域内稳定且带来源类别前缀的标识。
    /// @invariant 同一 AudioEndpointSnapshot 的 sources 中唯一。
    std::string id;

    /// @brief 面向用户的来源名称。
    std::string name;

    /// @brief 产生快照的平台后端名称，只用于展示和诊断。
    std::string backend;

    /// @brief 设备采集或应用输出类型。
    AudioSourceKind kind{ AudioSourceKind::DeviceInput };

    /// @brief 当前原生格式报告的通道数，未知时为零。
    std::uint32_t channels{};

    /// @brief 当前原生格式的标称采样率，未知时为零。
    std::uint32_t sampleRate{};

    /// @brief 系统是否将设备来源标记为默认录音端点。
    bool isDefault{};

    /// @brief 应用输出来源的身份；设备来源保持空值。
    std::optional<ApplicationIdentity> application;
};

/// @brief 消费路由混音结果的拥有型目标快照。
///
/// DeviceOutput 把数据写入真实播放流。VirtualMicrophone 在 AudioRoads 内仍是
/// 数据消费者，但平台组件会把同一数据作为系统录音来源发布给其他应用。
/// 目标不包含组件连接状态；端点离线由下一份快照中缺少稳定 ID 表达。
/// 平台组件的原生句柄、共享内存地址和线程均不得进入该值类型。
struct AudioTarget {
    /// @brief 后端作用域内稳定且带目标类别前缀的标识。
    /// @invariant 同一 AudioEndpointSnapshot 的 targets 中唯一。
    std::string id;

    /// @brief 面向用户的目标名称。
    std::string name;

    /// @brief 产生快照的平台后端名称，只用于展示和诊断。
    std::string backend;

    /// @brief 物理播放或虚拟麦克风类型。
    AudioTargetKind kind{ AudioTargetKind::DeviceOutput };

    /// @brief 目标当前接受的通道数，未知时为零。
    std::uint32_t channels{};

    /// @brief 目标当前采用的标称采样率，未知时为零。
    std::uint32_t sampleRate{};

    /// @brief 是否是系统默认播放目标；虚拟麦克风通常为 false。
    bool isDefault{};
};

/// @brief 一次平台发现操作产生的完整来源与目标快照。
///
/// 两个列表按路由引擎视角划分，而不是按操作系统设备方向划分；因此虚拟
/// 麦克风位于 targets，应用输出位于 sources。
/// 一次成功枚举必须同时提交两个列表，防止 UI 在刷新中观察跨代组合。
/// 列表内 ID 应唯一；平台后端负责在返回前合并同一稳定应用的多个进程。
struct AudioEndpointSnapshot {
    /// @brief 可被一条或多条路由共享读取的来源。
    std::vector<AudioSource> sources;

    /// @brief 接收一个或多个路由混音结果的目标。
    std::vector<AudioTarget> targets;
};

}  // namespace AudioRoads::Core
