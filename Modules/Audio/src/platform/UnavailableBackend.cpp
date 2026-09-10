#include "IAudioBackend.h"

#include <expected>
#include <memory>
#include <vector>

namespace AudioRoads::Audio
{
namespace
{

/// @brief 在无 PipeWire 开发包的构建机上提供明确错误，不伪造设备。
class UnavailableBackend final : public IAudioBackend
{
public:
    [[nodiscard]] const char* name() const noexcept override
    {
        return "PipeWire disabled";
    }

    [[nodiscard]] std::expected<std::vector<Core::AudioDevice>,
                                AudioBackendError>
    enumerateDevices() override
    {
        return std::unexpected(AudioBackendError{
            .operation = "初始化 PipeWire",
            .message   = "此构建显式关闭了 AUDIOROADS_ENABLE_PIPEWIRE" });
    }
};

}  // namespace

std::unique_ptr<IAudioBackend> createUnavailableBackend()
{
    return std::make_unique<UnavailableBackend>();
}

}  // namespace AudioRoads::Audio
