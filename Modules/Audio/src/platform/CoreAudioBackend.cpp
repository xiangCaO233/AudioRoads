#include "IAudioBackend.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AudioRoads::Audio
{
namespace
{

/// @brief 将 CoreFoundation 字符串复制为 UTF-8 值类型。
[[nodiscard]] std::string toUtf8(CFStringRef value)
{
    if ( value == nullptr ) return {};
    const auto length = CFStringGetLength(value);
    const auto capacity =
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<std::size_t>(capacity), '\0');
    if ( !CFStringGetCString(
             value, result.data(), capacity, kCFStringEncodingUTF8) ) {
        return {};
    }
    result.resize(std::char_traits<char>::length(result.c_str()));
    return result;
}

/// @brief 读取设备 CFString 属性并立即释放原生对象。
[[nodiscard]] std::string stringProperty(AudioDeviceID               device,
                                         AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address{ selector,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    CFStringRef                value{};
    UInt32                     size = sizeof(value);
    if ( AudioObjectGetPropertyData(
             device, &address, 0, nullptr, &size, &value) != noErr ) {
        return {};
    }
    auto result = toUtf8(value);
    if ( value != nullptr ) CFRelease(value);
    return result;
}

/// @brief 汇总指定 scope 下所有流的通道数。
[[nodiscard]] std::uint32_t channelCount(AudioDeviceID            device,
                                         AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address{ kAudioDevicePropertyStreamConfiguration,
                                        scope,
                                        kAudioObjectPropertyElementMain };
    UInt32                     size{};
    if ( AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) !=
         noErr ) {
        return 0;
    }

    std::vector<std::byte> storage(size);
    if ( AudioObjectGetPropertyData(
             device, &address, 0, nullptr, &size, storage.data()) != noErr ) {
        return 0;
    }

    const auto* list = reinterpret_cast<const AudioBufferList*>(storage.data());
    std::uint32_t channels{};
    for ( UInt32 index = 0; index < list->mNumberBuffers; ++index ) {
        channels += list->mBuffers[index].mNumberChannels;
    }
    return channels;
}

/// @brief 查询系统默认输入或输出设备 ID。
[[nodiscard]] AudioDeviceID defaultDevice(AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address{ selector,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    AudioDeviceID              device{};
    UInt32                     size = sizeof(device);
    if ( AudioObjectGetPropertyData(
             kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) !=
         noErr ) {
        return kAudioObjectUnknown;
    }
    return device;
}

/// @brief 使用 CoreAudio HAL 发现物理及虚拟音频设备。
class CoreAudioBackend final : public IAudioBackend
{
public:
    [[nodiscard]] const char* name() const noexcept override
    {
        return "CoreAudio";
    }

    [[nodiscard]] std::expected<std::vector<Core::AudioDevice>,
                                AudioBackendError>
    enumerateDevices() override;
};

std::expected<std::vector<Core::AudioDevice>, AudioBackendError>
CoreAudioBackend::enumerateDevices()
{
    AudioObjectPropertyAddress address{ kAudioHardwarePropertyDevices,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    UInt32                     size{};
    auto                       status = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject, &address, 0, nullptr, &size);
    if ( status != noErr ) {
        return std::unexpected(AudioBackendError{
            .operation = "读取 CoreAudio 设备数量",
            .message   = "OSStatus=" + std::to_string(status) });
    }

    std::vector<AudioDeviceID> nativeDevices(size / sizeof(AudioDeviceID));
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                        &address,
                                        0,
                                        nullptr,
                                        &size,
                                        nativeDevices.data());
    if ( status != noErr ) {
        return std::unexpected(AudioBackendError{
            .operation = "枚举 CoreAudio 设备",
            .message   = "OSStatus=" + std::to_string(status) });
    }

    const auto defaultInput =
        defaultDevice(kAudioHardwarePropertyDefaultInputDevice);
    const auto defaultOutput =
        defaultDevice(kAudioHardwarePropertyDefaultOutputDevice);
    std::vector<Core::AudioDevice> devices;
    devices.reserve(nativeDevices.size());

    for ( const auto nativeDevice : nativeDevices ) {
        const auto inputs =
            channelCount(nativeDevice, kAudioObjectPropertyScopeInput);
        const auto outputs =
            channelCount(nativeDevice, kAudioObjectPropertyScopeOutput);
        if ( inputs == 0 && outputs == 0 ) continue;

        auto uid = stringProperty(nativeDevice, kAudioDevicePropertyDeviceUID);
        if ( uid.empty() ) uid = std::to_string(nativeDevice);
        auto name = stringProperty(nativeDevice, kAudioObjectPropertyName);
        if ( name.empty() ) name = uid;

        Float64                    sampleRate{};
        AudioObjectPropertyAddress rateAddress{
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 rateSize = sizeof(sampleRate);
        AudioObjectGetPropertyData(
            nativeDevice, &rateAddress, 0, nullptr, &rateSize, &sampleRate);

        const auto flow = inputs > 0 && outputs > 0
                              ? Core::DeviceFlow::Duplex
                              : (inputs > 0 ? Core::DeviceFlow::Input
                                            : Core::DeviceFlow::Output);
        devices.push_back(Core::AudioDevice{
            .id             = "coreaudio:" + uid,
            .name           = std::move(name),
            .backend        = "CoreAudio",
            .flow           = flow,
            .inputChannels  = inputs,
            .outputChannels = outputs,
            .sampleRate     = static_cast<std::uint32_t>(sampleRate),
            .isDefault =
                nativeDevice == defaultInput || nativeDevice == defaultOutput,
        });
    }
    return devices;
}

}  // namespace

std::unique_ptr<IAudioBackend> createCoreAudioBackend()
{
    return std::make_unique<CoreAudioBackend>();
}

}  // namespace AudioRoads::Audio
