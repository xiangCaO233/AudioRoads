#include "IAudioBackend.h"

#include <expected>
#include <memory>
#include <vector>

namespace AudioRoads::Audio
{
namespace
{

/// @brief 在显式关闭 PipeWire 的开发构建中提供可观测失败。
///
/// 该对象不建立线程或平台句柄，也绝不伪造默认设备或静默回退 ALSA；产品
/// Linux 构建应启用 PipeWire。稳定名称让 UI 能明确展示当前构建能力。
class UnavailableBackend final : public IAudioBackend
{
public:
    /// @brief 返回明确包含 disabled 的能力标签，避免 UI 误报已连接后端。
    [[nodiscard]] const char* name() const noexcept override
    {
        // 静态字面量满足接口返回文本在后端对象寿命内有效的约束。
        return "PipeWire disabled";
    }

    [[nodiscard]] std::expected<Core::AudioEndpointSnapshot, AudioBackendError>
    enumerateEndpoints() override
    {
        // 不返回空成功：空集合可能被误解为“服务正常但没有设备”，必须让构建
        // 配置问题沿标准错误通道抵达 UI。
        // 指向产生该状态的配置开关，便于开发者从 UI 错误直接恢复构建配置。
        return std::unexpected(AudioBackendError{
            // operation 与正常 PipeWire 初始化阶段同名，UI
            // 可直接组合统一错误格式。
            .operation = "初始化 PipeWire",
            .message   = "此构建显式关闭了 AUDIOROADS_ENABLE_PIPEWIRE" });
    }
};

}  // namespace

std::unique_ptr<IAudioBackend> createUnavailableBackend()
{
    // 仍通过接口工厂交付唯一所有权，使 AudioService 不需要特殊空值分支。
    return std::make_unique<UnavailableBackend>();
}

}  // namespace AudioRoads::Audio
