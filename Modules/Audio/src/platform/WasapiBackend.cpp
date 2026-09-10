#include "IAudioBackend.h"

#define NOMINMAX
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propvarutil.h>
#include <wrl/client.h>

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

using Microsoft::WRL::ComPtr;

/// @brief 把 COM 分配的 UTF-16 文本转换为内部 UTF-8 标识或名称。
[[nodiscard]] std::string toUtf8(const wchar_t* value)
{
    if ( value == nullptr || *value == L'\0' ) return {};
    const auto size = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if ( size <= 1 ) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

/// @brief 读取 MMDevice 的用户可见名称，失败时使用稳定 ID。
[[nodiscard]] std::string deviceName(IMMDevice&         device,
                                     const std::string& fallback)
{
    ComPtr<IPropertyStore> properties;
    if ( FAILED(device.OpenPropertyStore(STGM_READ, &properties)) )
        return fallback;

    PROPVARIANT value;
    PropVariantInit(&value);
    const auto  result = properties->GetValue(PKEY_Device_FriendlyName, &value);
    std::string name   = fallback;
    if ( SUCCEEDED(result) && value.vt == VT_LPWSTR )
        name = toUtf8(value.pwszVal);
    PropVariantClear(&value);
    return name;
}

/// @brief 查询端点共享模式混合格式，缺失时保留零值供 UI 展示未知。
void populateMixFormat(IMMDevice& device, Core::AudioDevice& result)
{
    ComPtr<IAudioClient> client;
    if ( FAILED(device.Activate(
             IID_IAudioClient, CLSCTX_ALL, nullptr, &client)) ) {
        return;
    }

    WAVEFORMATEX* format{};
    if ( FAILED(client->GetMixFormat(&format)) || format == nullptr ) return;
    result.sampleRate = format->nSamplesPerSec;
    if ( Core::canCapture(result) ) result.inputChannels = format->nChannels;
    if ( Core::canRender(result) ) result.outputChannels = format->nChannels;
    CoTaskMemFree(format);
}

/// @brief 枚举一个数据流方向并附加到统一设备快照。
[[nodiscard]] HRESULT enumerateFlow(IMMDeviceEnumerator& enumerator,
                                    EDataFlow nativeFlow, Core::DeviceFlow flow,
                                    std::vector<Core::AudioDevice>& output)
{
    ComPtr<IMMDeviceCollection> collection;
    auto                        result = enumerator.EnumAudioEndpoints(
        nativeFlow, DEVICE_STATE_ACTIVE, &collection);
    if ( FAILED(result) ) return result;

    ComPtr<IMMDevice> defaultDevice;
    std::string       defaultId;
    if ( SUCCEEDED(enumerator.GetDefaultAudioEndpoint(
             nativeFlow, eMultimedia, &defaultDevice)) ) {
        wchar_t* nativeId{};
        if ( SUCCEEDED(defaultDevice->GetId(&nativeId)) ) {
            defaultId = toUtf8(nativeId);
            CoTaskMemFree(nativeId);
        }
    }

    UINT count{};
    result = collection->GetCount(&count);
    if ( FAILED(result) ) return result;

    for ( UINT index = 0; index < count; ++index ) {
        ComPtr<IMMDevice> device;
        if ( FAILED(collection->Item(index, &device)) ) continue;

        wchar_t* nativeId{};
        if ( FAILED(device->GetId(&nativeId)) || nativeId == nullptr ) continue;
        auto id = toUtf8(nativeId);
        CoTaskMemFree(nativeId);

        Core::AudioDevice snapshot{ .id        = "wasapi:" + id,
                                    .name      = deviceName(*device.Get(), id),
                                    .backend   = "WASAPI",
                                    .flow      = flow,
                                    .isDefault = id == defaultId };
        populateMixFormat(*device.Get(), snapshot);
        output.push_back(std::move(snapshot));
    }
    return S_OK;
}

/// @brief 使用 MMDevice 枚举端点，并为后续 WASAPI 流保留同一 ID 空间。
class WasapiBackend final : public IAudioBackend
{
public:
    /// @brief 在当前线程初始化多线程 COM apartment。
    WasapiBackend()
    {
        const auto result       = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_ownsComInitialization = SUCCEEDED(result);
        m_comAvailable = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }

    /// @brief 只平衡由本实例成功完成的 COM 初始化。
    ~WasapiBackend() override
    {
        if ( m_ownsComInitialization ) CoUninitialize();
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "WASAPI";
    }

    [[nodiscard]] std::expected<std::vector<Core::AudioDevice>,
                                AudioBackendError>
    enumerateDevices() override;

private:
    /// @brief COM 是否可在当前线程使用。
    bool m_comAvailable{};

    /// @brief 是否需要在析构时调用 CoUninitialize。
    bool m_ownsComInitialization{};
};

std::expected<std::vector<Core::AudioDevice>, AudioBackendError>
WasapiBackend::enumerateDevices()
{
    if ( !m_comAvailable ) {
        return std::unexpected(AudioBackendError{
            .operation = "初始化 WASAPI", .message = "COM apartment 不可用" });
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    auto result = CoCreateInstance(CLSID_MMDeviceEnumerator,
                                   nullptr,
                                   CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator));
    if ( FAILED(result) ) {
        return std::unexpected(AudioBackendError{
            .operation = "创建 MMDevice 枚举器",
            .message   = "HRESULT=" + std::to_string(result) });
    }

    std::vector<Core::AudioDevice> devices;
    result = enumerateFlow(
        *enumerator.Get(), eCapture, Core::DeviceFlow::Input, devices);
    if ( SUCCEEDED(result) ) {
        result = enumerateFlow(
            *enumerator.Get(), eRender, Core::DeviceFlow::Output, devices);
    }
    if ( FAILED(result) ) {
        return std::unexpected(AudioBackendError{
            .operation = "枚举 WASAPI 端点",
            .message   = "HRESULT=" + std::to_string(result) });
    }
    return devices;
}

}  // namespace

std::unique_ptr<IAudioBackend> createWasapiBackend()
{
    return std::make_unique<WasapiBackend>();
}

}  // namespace AudioRoads::Audio
