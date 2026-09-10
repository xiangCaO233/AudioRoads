#include "IAudioBackend.h"

#define NOMINMAX
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
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

/// @brief 关闭只用于查询应用身份的 Windows 进程句柄。
///
/// 进程可能在会话枚举与路径查询之间退出，此类只保证句柄配平，不把退出视为
/// 异常。句柄永不进入 Core DTO，也不会由音频实时线程打开或关闭。
class ProcessHandle final
{
public:
    /// @brief 接管一个可空 HANDLE；调用方不得在此后再次 CloseHandle。
    explicit ProcessHandle(HANDLE value) noexcept : m_value(value) {}

    /// @brief 仅关闭成功取得的句柄，空值无需平台调用。
    ~ProcessHandle()
    {
        if ( m_value != nullptr ) CloseHandle(m_value);
    }

    ProcessHandle(const ProcessHandle&)            = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;

    /// @brief 返回借用句柄，只允许在当前所有者寿命内同步使用。
    [[nodiscard]] HANDLE get() const noexcept { return m_value; }

private:
    /// @brief 由本对象唯一拥有的 Windows 内核句柄。
    HANDLE m_value{};
};

/// @brief MMDevice 转为 Core 来源或目标前的公共属性快照。
///
/// 中间类型使同一枚举函数可以处理 capture/render 两个方向，同时避免把
/// EDataFlow 写入 Core。所有字段都拥有值，不保留 IMMDevice 或格式指针。
struct NativeEndpointSnapshot {
    /// @brief 尚未添加 AudioRoads 类别前缀的 MMDevice ID。
    std::string id;
    /// @brief 用户可见名称，属性缺失时退回原生 ID。
    std::string name;
    /// @brief 共享混合格式的通道数，激活失败时为零。
    std::uint32_t channels{};
    /// @brief 共享混合格式的采样率，激活失败时为零。
    std::uint32_t sampleRate{};
    /// @brief 是否匹配本方向 eMultimedia 默认端点。
    bool isDefault{};
};

/// @brief 把 COM 返回的 UTF-16 文本转换为内部 UTF-8 标识或名称。
///
/// 先询问包含结尾空字符的所需长度，再进行一次完整转换；任何失败都返回空值，
/// 不把部分 UTF-8 字节带入稳定 ID。原生缓冲区的释放仍由调用者负责。
/// @param value 借用、以空字符结尾的 Windows UTF-16 文本。
/// @return 拥有型 UTF-8 字符串；空输入或转换失败时为空。
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

/// @brief 读取当前进程实例对应的稳定可执行文件路径。
/// @details 查询只申请最低必要权限，受保护进程或已退出进程读取失败时返回空值。
/// 路径仅用作同一 Windows 安装内的应用聚合键，不承诺跨安装位置稳定。
/// @param processId 音频会话报告的当前 PID。
[[nodiscard]] std::string processPath(DWORD processId)
{
    ProcessHandle process{ OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId) };
    // 权限拒绝与进程退出都是单会话可恢复情况，由调用方退回 PID 字符串。
    if ( process.get() == nullptr ) return {};

    // QueryFullProcessImageName 支持长路径，缓冲区仅存在于低频枚举阶段。
    std::wstring path(32'768, L'\0');
    DWORD        pathSize = static_cast<DWORD>(path.size());
    if ( QueryFullProcessImageNameW(process.get(), 0, path.data(), &pathSize) ==
         FALSE ) {
        return {};
    }
    path.resize(pathSize);
    return toUtf8(path.c_str());
}

/// @brief 从稳定应用路径提取适合 UI 展示的文件名。
/// @details 同时接受 Windows 与 Unix 分隔符，便于测试和未来包标识降级值。
/// 路径和 PID 都不可用的情况不会发生在当前调用点，仍提供固定前缀防御空名称。
[[nodiscard]] std::string applicationName(const std::string& stableId,
                                          DWORD              processId)
{
    const auto separator = stableId.find_last_of("\\/");
    if ( separator != std::string::npos && separator + 1 < stableId.size() ) {
        return stableId.substr(separator + 1);
    }
    if ( !stableId.empty() ) return stableId;
    return "Process " + std::to_string(processId);
}

/// @brief 读取 MMDevice 的用户可见名称，失败时使用稳定 ID。
///
/// 友好名称属于可选展示属性，读取失败不应中断整个端点集合。PROPVARIANT 的
/// 内部所有权无论类型是否符合预期都由本函数清理。
/// @param device 只在本次同步属性读取期间借用的活动 MMDevice。
/// @param fallback 名称属性缺失或类型不匹配时使用的原生 ID。
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
/// @param device 当前枚举项，函数不延长其 COM 生命周期。
/// @param result 接收可选格式字段，失败时保留调用方已有值。
void populateMixFormat(IMMDevice& device, NativeEndpointSnapshot& result)
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
    result.channels   = format->nChannels;
    CoTaskMemFree(format);
}

/// @brief 枚举一个数据流方向并附加到统一设备快照。
///
/// 采集和播放分别调用以保留明确方向。集合级 API 失败会返回 HRESULT 并让调用
/// 方拒绝整份快照；单个设备的可选名称或格式失败只使用稳定降级值。
/// @param output 成功解析的 DTO 追加目标，所有原生字符串均在追加前复制。
/// @param enumerator 当前 COM apartment 内借用的系统端点枚举器。
/// @param nativeFlow eCapture 或 eRender；调用方负责转换成 Core 强类型集合。
/// @return 集合级结果；局部设备热拔插不会升级成整体失败。
[[nodiscard]] HRESULT enumerateFlow(IMMDeviceEnumerator& enumerator,
                                    EDataFlow            nativeFlow,
                                    std::vector<NativeEndpointSnapshot>& output)
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
            // GetId 使用 COM task allocator，不能用 delete 或进程堆释放。
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
        // 稳定 ID 是后续重新打开流的唯一原生键；缺失时无法安全保留该端点。
        if ( FAILED(device->GetId(&nativeId)) || nativeId == nullptr ) continue;
        // GetId 返回 CoTaskMem 所有权，转为 UTF-8 后立即释放原生缓冲区。
        auto id = toUtf8(nativeId);
        CoTaskMemFree(nativeId);
        // ID 添加 backend 前缀，防止未来聚合多后端时原生 ID 命名空间碰撞。

        // 方向尚未进入中间快照，避免同一物理双工设备在这里被错误合并。
        NativeEndpointSnapshot snapshot{ .id   = id,
                                         .name = deviceName(*device.Get(), id),
                                         .isDefault = id == defaultId };
        // 友好名读取失败时 deviceName 使用无前缀原生 ID，便于用户排查系统设备。
        // 可选格式失败保留零通道/采样率；稳定 ID 和方向仍足以展示与后续重解析。
        populateMixFormat(*device.Get(), snapshot);
        output.push_back(std::move(snapshot));
    }
    // 所有集合和端点 ComPtr 在函数返回时释放，output 中不保留 COM 引用。
    return S_OK;
}

/// @brief 从所有活动播放端点聚合当前正在输出音频的桌面应用。
///
/// 音频会话只用于发现进程身份；真正采集由 Windows application loopback 以
/// processId 建立，不缓存 IAudioSessionControl。相同可执行文件的多个会话在
/// 控制面合并为一个应用来源。
/// @param enumerator 借用的系统端点枚举器，用于遍历全部活动 render 设备。
/// @param output 先含设备输入来源的最终容器；函数向其中追加应用来源。
/// @return render 集合级 COM 失败；单会话消失、权限拒绝或无 PID 时继续枚举。
[[nodiscard]] HRESULT enumerateApplicationSources(
    IMMDeviceEnumerator& enumerator, std::vector<Core::AudioSource>& output)
{
    ComPtr<IMMDeviceCollection> renderDevices;
    // 应用会话隶属于 render 端点而非全局集合，因此无法只查询默认设备一次。
    auto result = enumerator.EnumAudioEndpoints(
        eRender, DEVICE_STATE_ACTIVE, &renderDevices);
    if ( FAILED(result) ) return result;

    UINT deviceCount{};
    // 集合数量读取失败意味着无法证明应用快照完整，必须把错误交回服务层。
    result = renderDevices->GetCount(&deviceCount);
    if ( FAILED(result) ) return result;

    for ( UINT deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex ) {
        ComPtr<IMMDevice> device;
        // 默认播放设备之外的应用同样可以活动，必须遍历全部 render 端点会话。
        if ( FAILED(renderDevices->Item(deviceIndex, &device)) ) continue;

        NativeEndpointSnapshot mixFormat;
        // 会话 API 不直接报告 PCM 格式，暂用其所属 render 端点共享格式作展示。
        // 真正 application loopback 初始化时必须以返回的激活格式为准。
        populateMixFormat(*device.Get(), mixFormat);

        ComPtr<IAudioSessionManager2> manager;
        // SessionManager2 只用于控制面枚举；它不提供 application loopback
        // 样本。
        if ( FAILED(device->Activate(__uuidof(IAudioSessionManager2),
                                     CLSCTX_ALL,
                                     nullptr,
                                     &manager)) ) {
            continue;
        }

        ComPtr<IAudioSessionEnumerator> sessions;
        // 每个 render 端点拥有独立会话枚举器，离开本轮端点后全部 COM 引用释放。
        if ( FAILED(manager->GetSessionEnumerator(&sessions)) ) continue;

        int sessionCount{};
        // 单个端点的会话管理器不可用时跳过该端点，不污染其他端点已有结果。
        if ( FAILED(sessions->GetCount(&sessionCount)) ) continue;
        for ( int sessionIndex = 0; sessionIndex < sessionCount;
              ++sessionIndex ) {
            ComPtr<IAudioSessionControl> session;
            // 会话可在枚举期间结束，单项失败不能清空此前已发现的其他应用。
            if ( FAILED(sessions->GetSession(sessionIndex, &session)) )
                continue;

            AudioSessionState sessionState{};
            // 只暴露 Active 会话，Inactive/Expired
            // 软件没有可供当前路由捕获的数据。
            if ( FAILED(session->GetState(&sessionState)) ||
                 sessionState != AudioSessionStateActive ) {
                continue;
            }

            ComPtr<IAudioSessionControl2> session2;
            // 系统声音没有普通进程身份且可能包含敏感通知，单应用列表明确排除。
            if ( FAILED(session.As(&session2)) ||
                 session2->IsSystemSoundsSession() == S_OK ) {
                continue;
            }

            DWORD processId{};
            if ( FAILED(session2->GetProcessId(&processId)) || processId == 0 ||
                 processId == GetCurrentProcessId() ) {
                // 排除本进程防止未来捕获 AudioRoads 自身播放形成反馈环。
                continue;
            }

            auto stableId = processPath(processId);
            // 路径读取受权限限制时以 PID
            // 保留临时可选来源，但刷新后可能无法恢复。
            if ( stableId.empty() ) stableId = std::to_string(processId);
            const auto sourceId = "wasapi:application:" + stableId;
            // stableId 用于把浏览器等多会话软件显示成单个方块；每个当前 PID
            // 仍保存在 identity 中，建立捕获时需要分别打开并混合。
            const auto duplicate =
                std::ranges::find(output, sourceId, &Core::AudioSource::id);
            if ( duplicate != output.end() ) {
                // 一个软件的多个活动会话/进程共同组成应用来源，捕获阶段需全接入。
                auto& processIds = duplicate->application->processIds;
                if ( std::ranges::find(processIds, processId) ==
                     processIds.end() ) {
                    processIds.push_back(processId);
                }
                continue;
            }

            wchar_t*    nativeDisplayName{};
            std::string displayName;
            // GetDisplayName 的空成功值也需要降级；非空缓冲由 COM allocator
            // 拥有。
            if ( SUCCEEDED(session->GetDisplayName(&nativeDisplayName)) &&
                 nativeDisplayName != nullptr ) {
                displayName = toUtf8(nativeDisplayName);
                CoTaskMemFree(nativeDisplayName);
            }
            if ( displayName.empty() ) {
                // 会话通常没有显式名称，用可执行文件名比显示整条路径更适合节点。
                displayName = applicationName(stableId, processId);
            }

            output.push_back(Core::AudioSource{
                .id         = sourceId,
                .name       = std::move(displayName),
                .backend    = "WASAPI",
                .kind       = Core::AudioSourceKind::ApplicationOutput,
                .channels   = mixFormat.channels,
                .sampleRate = mixFormat.sampleRate,
                .isDefault  = false,
                .application =
                    Core::ApplicationIdentity{ .processIds = { processId },
                                               .stableId =
                                                   std::move(stableId) },
            });
        }
    }
    return S_OK;
}

/// @brief 使用 MMDevice 枚举端点，并为后续 WASAPI 流保留同一 ID 空间。
/// @warning 枚举是低频同步控制面操作，不能从实时回调调用。
///
/// 对象只保存当前线程 COM apartment 的可用性与释放责任，不缓存枚举器或设备
/// 接口。每次刷新都会返回完全拥有字符串的来源与目标值集合。
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

    [[nodiscard]] std::expected<Core::AudioEndpointSnapshot, AudioBackendError>
    enumerateEndpoints() override;

private:
    /// @brief COM 是否可在当前线程使用。
    bool m_comAvailable{};

    /// @brief 是否需要在析构时调用 CoUninitialize。
    bool m_ownsComInitialization{};
};

std::expected<Core::AudioEndpointSnapshot, AudioBackendError>
WasapiBackend::enumerateEndpoints()
{
    // 所有 COM 操作都在构造后端的控制线程同步执行；未来设备通知只能投递动作，
    // 不能从任意 COM 回调直接修改 RoutingGraph。
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

    std::vector<NativeEndpointSnapshot> captureDevices;
    std::vector<NativeEndpointSnapshot> renderDevices;
    // 分方向枚举后再映射到强类型列表，避免把操作系统方向误当作路由方向。
    result = enumerateFlow(*enumerator.Get(), eCapture, captureDevices);
    if ( SUCCEEDED(result) ) {
        // 仅当采集集合完整成功才继续播放集合，保持返回快照的两方向一致性。
        result = enumerateFlow(*enumerator.Get(), eRender, renderDevices);
    }
    if ( FAILED(result) ) {
        // 任一方向发生集合级失败都拒绝部分快照，AudioService 会保留旧设备集合。
        return std::unexpected(AudioBackendError{
            .operation = "枚举 WASAPI 端点",
            .message   = "HRESULT=" + std::to_string(result) });
    }
    Core::AudioEndpointSnapshot endpoints;
    // capture 设备映射为 source，render 设备映射为 target；双工硬件会自然形成
    // 两个具有不同类别前缀的稳定 ID，Core 不再推断操作系统流方向。
    endpoints.sources.reserve(captureDevices.size());
    endpoints.targets.reserve(renderDevices.size());
    for ( auto& device : captureDevices ) {
        // 捕获设备只进入来源列，即使其物理硬件同时具有播放通道也不在 DTO 合并。
        endpoints.sources.push_back(Core::AudioSource{
            .id          = "wasapi:device-input:" + device.id,
            .name        = std::move(device.name),
            .backend     = "WASAPI",
            .kind        = Core::AudioSourceKind::DeviceInput,
            .channels    = device.channels,
            .sampleRate  = device.sampleRate,
            .isDefault   = device.isDefault,
            .application = std::nullopt,
        });
    }
    for ( auto& device : renderDevices ) {
        // 播放设备是数据消费者；后续目标回调负责从所有关联来源读取并混音。
        endpoints.targets.push_back(Core::AudioTarget{
            .id         = "wasapi:device-output:" + device.id,
            .name       = std::move(device.name),
            .backend    = "WASAPI",
            .kind       = Core::AudioTargetKind::DeviceOutput,
            .channels   = device.channels,
            .sampleRate = device.sampleRate,
            .isDefault  = device.isDefault,
        });
    }
    result = enumerateApplicationSources(*enumerator.Get(), endpoints.sources);
    if ( FAILED(result) ) {
        // 应用集合级失败时保留上一整份服务快照，不能只显示物理设备造成假成功。
        return std::unexpected(AudioBackendError{
            .operation = "枚举 WASAPI 应用音频会话",
            .message   = "HRESULT=" + std::to_string(result) });
    }
    // 返回后枚举器释放；未来打开流必须用稳定 ID 重新取得当前 IMMDevice。
    return endpoints;
}

}  // namespace

std::unique_ptr<IAudioBackend> createWasapiBackend()
{
    // 具体类型留在平台翻译单元，工厂只向跨平台服务暴露接口所有权。
    return std::make_unique<WasapiBackend>();
}

}  // namespace AudioRoads::Audio
