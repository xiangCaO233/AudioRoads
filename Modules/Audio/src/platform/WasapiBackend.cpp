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

/// @brief 把 COM 返回的 UTF-16 文本转换为内部 UTF-8 标识或名称。
///
/// 先询问包含结尾空字符的所需长度，再进行一次完整转换；任何失败都返回空值，
/// 不把部分 UTF-8 字节带入稳定 ID。原生缓冲区的释放仍由调用者负责。
[[nodiscard]] std::string toUtf8(const wchar_t* value)
{
    if ( value == nullptr || *value == L'\0' ) return {};
    // CP_UTF8 不依赖当前 Windows ANSI code page，设备 ID 在不同区域设置下稳定。
    const auto size = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if ( size <= 1 ) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    // std::string 不保存 API 写入的结尾空字符，尺寸恢复为实际 UTF-8 字节数。
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

/// @brief 读取 MMDevice 的用户可见名称，失败时使用稳定 ID。
///
/// 友好名称属于可选展示属性，读取失败不应中断整个端点集合。PROPVARIANT 的
/// 内部所有权无论类型是否符合预期都由本函数清理。
[[nodiscard]] std::string deviceName(IMMDevice&         device,
                                     const std::string& fallback)
{
    ComPtr<IPropertyStore> properties;
    // 属性存储不可用只影响可读名称，稳定设备 ID 已由调用方提供作为 fallback。
    if ( FAILED(device.OpenPropertyStore(STGM_READ, &properties)) )
        return fallback;

    PROPVARIANT value;
    PropVariantInit(&value);
    const auto  result = properties->GetValue(PKEY_Device_FriendlyName, &value);
    std::string name   = fallback;
    if ( SUCCEEDED(result) && value.vt == VT_LPWSTR )
        name = toUtf8(value.pwszVal);
    // UTF-8 转换失败会得到空展示值，但不会影响调用方已取得的稳定设备 ID。
    // PROPVARIANT 可能拥有内部字符串，即使读取失败也必须统一清理。
    PropVariantClear(&value);
    return name;
}

/// @brief 查询端点共享模式混合格式，缺失时保留零值供 UI 展示未知。
///
/// IAudioClient 仅用于一次属性查询，不打开流；ComPtr 管理接口，GetMixFormat
/// 返回的 CoTaskMem 缓冲区则在复制采样率和通道数后立即释放。
void populateMixFormat(IMMDevice& device, Core::AudioDevice& result)
{
    ComPtr<IAudioClient> client;
    if ( FAILED(device.Activate(
             IID_IAudioClient, CLSCTX_ALL, nullptr, &client)) ) {
        // 单个设备无法激活不影响其作为端点显示，能力字段维持未知零值。
        return;
    }

    WAVEFORMATEX* format{};
    if ( FAILED(client->GetMixFormat(&format)) || format == nullptr ) return;
    // 混合格式仅是展示快照；真正打开流时必须重新协商，不能缓存该指针。
    result.sampleRate = format->nSamplesPerSec;
    // 枚举调用已确定 flow，只向对应方向写通道数，保持跨平台 DTO 语义一致。
    if ( Core::canCapture(result) ) result.inputChannels = format->nChannels;
    if ( Core::canRender(result) ) result.outputChannels = format->nChannels;
    CoTaskMemFree(format);
}

/// @brief 枚举一个数据流方向并附加到统一设备快照。
///
/// 采集和播放分别调用以保留明确方向。集合级 API 失败会返回 HRESULT 并让调用
/// 方拒绝整份快照；单个设备的可选名称或格式失败只使用稳定降级值。
/// @param output 成功解析的 DTO 追加目标，所有原生字符串均在追加前复制。
[[nodiscard]] HRESULT enumerateFlow(IMMDeviceEnumerator& enumerator,
                                    EDataFlow nativeFlow, Core::DeviceFlow flow,
                                    std::vector<Core::AudioDevice>& output)
{
    ComPtr<IMMDeviceCollection> collection;
    // 只暴露 ACTIVE 端点，避免用户创建指向禁用或未插入设备的路由。
    auto result = enumerator.EnumAudioEndpoints(
        nativeFlow, DEVICE_STATE_ACTIVE, &collection);
    if ( FAILED(result) ) return result;

    ComPtr<IMMDevice> defaultDevice;
    std::string       defaultId;
    // eMultimedia 对应通用客户端语义；若以后增加 role，需单独定义默认标志规则。
    if ( SUCCEEDED(enumerator.GetDefaultAudioEndpoint(
             nativeFlow, eMultimedia, &defaultDevice)) ) {
        wchar_t* nativeId{};
        if ( SUCCEEDED(defaultDevice->GetId(&nativeId)) ) {
            // 默认设备 ID 只用于本方向的值比较，不将 IMMDevice 指针写入快照。
            defaultId = toUtf8(nativeId);
            CoTaskMemFree(nativeId);
        }
    }

    UINT count{};
    // 默认端点查询失败不妨碍枚举，只会让此方向所有 isDefault 保持 false。
    result = collection->GetCount(&count);
    // 数量读取是集合级契约，失败时不能安全判断快照是否完整。
    if ( FAILED(result) ) return result;

    for ( UINT index = 0; index < count; ++index ) {
        ComPtr<IMMDevice> device;
        // 一个端点在枚举期间消失属于局部竞态，跳过它而不丢弃其他活动端点。
        if ( FAILED(collection->Item(index, &device)) ) continue;

        wchar_t* nativeId{};
        if ( FAILED(device->GetId(&nativeId)) || nativeId == nullptr ) continue;
        // GetId 返回 CoTaskMem 所有权，转为 UTF-8 后立即释放原生缓冲区。
        auto id = toUtf8(nativeId);
        CoTaskMemFree(nativeId);
        // ID 添加 backend 前缀，防止未来聚合多后端时原生 ID 命名空间碰撞。

        Core::AudioDevice snapshot{ .id        = "wasapi:" + id,
                                    .name      = deviceName(*device.Get(), id),
                                    .backend   = "WASAPI",
                                    .flow      = flow,
                                    .isDefault = id == defaultId };
        // 友好名读取失败时 deviceName 使用无前缀原生 ID，便于用户排查系统设备。
        // 可选格式失败保留零通道/采样率；稳定 ID 和方向仍足以展示与后续重解析。
        populateMixFormat(*device.Get(), snapshot);
        output.push_back(std::move(snapshot));
    }
    // 所有集合和端点 ComPtr 在函数返回时释放，output 中不保留 COM 引用。
    return S_OK;
}

/// @brief 使用 MMDevice 枚举端点，并为后续 WASAPI 流保留同一 ID 空间。
/// @warning 枚举是低频同步控制面操作，不能从实时回调调用。
///
/// 对象只保存当前线程 COM apartment 的可用性与释放责任，不缓存枚举器或设备
/// 接口。每次刷新都会返回完全拥有字符串的 Core::AudioDevice 值集合。
class WasapiBackend final : public IAudioBackend
{
public:
    /// @brief 在当前线程初始化多线程 COM apartment。
    WasapiBackend()
    {
        // 后端与调用线程的 apartment 责任绑定，因此构造、枚举和析构应在同线程。
        const auto result       = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_ownsComInitialization = SUCCEEDED(result);
        // apartment 已由宿主以其他模型建立时 COM
        // 仍可用，但本对象没有反初始化权。
        m_comAvailable = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }

    /// @brief 只平衡由本实例成功完成的 COM 初始化。
    ~WasapiBackend() override
    {
        // RPC_E_CHANGED_MODE 的 apartment
        // 由外部建立，本实例不得替别人反初始化。
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
        // apartment 初始化既未成功也不是已存在模式，继续创建 COM 对象没有意义。
        return std::unexpected(AudioBackendError{
            .operation = "初始化 WASAPI", .message = "COM apartment 不可用" });
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    // 枚举器是单次刷新资源，不缓存它即可让系统设备变化在下一次调用可见。
    auto result = CoCreateInstance(CLSID_MMDeviceEnumerator,
                                   nullptr,
                                   CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator));
    if ( FAILED(result) ) {
        // HRESULT 保留原始数值，避免用模糊文本掩盖权限、服务或注册问题。
        return std::unexpected(AudioBackendError{
            .operation = "创建 MMDevice 枚举器",
            .message   = "HRESULT=" + std::to_string(result) });
    }

    std::vector<Core::AudioDevice> devices;
    // 两个方向追加到同一值容器，但每个 DTO 仍携带明确 flow 与 backend 前缀。
    // 分方向枚举以保留 DTO 的单向语义；任一集合级失败都拒绝提交部分快照。
    result = enumerateFlow(
        *enumerator.Get(), eCapture, Core::DeviceFlow::Input, devices);
    if ( SUCCEEDED(result) ) {
        // 仅当采集集合完整成功才继续播放集合，保持返回快照的两方向一致性。
        result = enumerateFlow(
            *enumerator.Get(), eRender, Core::DeviceFlow::Output, devices);
    }
    if ( FAILED(result) ) {
        // 任一方向发生集合级失败都拒绝部分快照，AudioService 会保留旧设备集合。
        return std::unexpected(AudioBackendError{
            .operation = "枚举 WASAPI 端点",
            .message   = "HRESULT=" + std::to_string(result) });
    }
    // 所有 ComPtr 在返回时释放，结果只包含跨层安全的 UTF-8 值和标量。
    // 返回后枚举器释放；未来打开流必须用稳定 ID 重新取得当前 IMMDevice。
    return devices;
}

}  // namespace

std::unique_ptr<IAudioBackend> createWasapiBackend()
{
    // 具体类型留在平台翻译单元，工厂只向跨平台服务暴露接口所有权。
    return std::make_unique<WasapiBackend>();
}

}  // namespace AudioRoads::Audio
