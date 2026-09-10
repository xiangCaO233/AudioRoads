#pragma once

#include "AudioTypes.h"

#include <expected>
#include <string>
#include <vector>

namespace AudioRoads::Audio
{

/// @brief 平台音频后端返回的可展示错误。
///
/// 错误不是稳定协议，只用于当前运行时诊断和 UI 展示。
/// 调用方不得持久化文本或使用它驱动程序分支。
struct AudioBackendError {
    /// @brief 失败所属的平台操作，例如连接服务或枚举端点。
    std::string operation;

    /// @brief 原生错误码或上下文信息的可读描述。
    std::string message;
};

/// @brief 隔离设备发现和未来流控制的平台边界。
///
/// 后端只返回 Core 值类型，不向上层泄漏 COM、PipeWire 或 HAL 句柄。
/// 对象由 AudioService 独占持有，经虚析构释放平台资源。
/// 新增后端必须同时接入工厂和 CMake 平台源文件选择。
/// 接口只依赖 Core，不允许反向依赖 UI 或 Main。
class IAudioBackend
{
public:
    virtual ~IAudioBackend() = default;

    /// @brief 返回用于诊断和 UI 展示的后端名称。
    /// @return 指向后端寿命内有效的静态文本，不转移字符串所有权。
    /// @note 名称不能用于决定平台逻辑，工厂已在编译期完成选择。
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// @brief 同步取得当前活动音频端点快照。
    ///
    /// 此操作只允许在启动、用户刷新或设备通知处理阶段调用，禁止从实时音频
    /// 回调调用。后续流控制仍使用同一后端对象和设备稳定 ID。
    /// 成功值必须是完整快照，不是对旧列表的增量补丁。
    /// 设备 ID 必须带后端前缀，名称缺失时可以降级为 ID。
    /// 返回 DTO 必须拥有全部字符串，不能借用平台枚举回调内存。
    /// 单个可选展示属性可降级，但集合级失败不得伪装为空成功。
    /// 可恢复失败通过 expected 返回，后端实现不得抛出异常。
    [[nodiscard]] virtual std::expected<std::vector<Core::AudioDevice>,
                                        AudioBackendError>
    enumerateDevices() = 0;
};

}  // namespace AudioRoads::Audio
