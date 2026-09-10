#pragma once

#include "AudioTypes.h"

#include <expected>
#include <string>
#include <vector>

namespace AudioRoads::Audio
{

/// @brief 平台音频后端返回的可展示错误。
struct AudioBackendError {
    /// @brief 失败所属的平台操作，例如连接服务或枚举端点。
    std::string operation;

    /// @brief 原生错误码或上下文信息的可读描述。
    std::string message;
};

/// @brief 隔离设备发现和未来流控制的平台边界。
class IAudioBackend
{
public:
    virtual ~IAudioBackend() = default;

    /// @brief 返回用于诊断和 UI 展示的后端名称。
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// @brief 同步取得当前活动音频端点快照。
    ///
    /// 此操作只允许在启动、用户刷新或设备通知处理阶段调用，禁止从实时音频
    /// 回调调用。后续流控制仍使用同一后端对象和设备稳定 ID。
    [[nodiscard]] virtual std::expected<std::vector<Core::AudioDevice>,
                                        AudioBackendError>
    enumerateDevices() = 0;
};

}  // namespace AudioRoads::Audio
