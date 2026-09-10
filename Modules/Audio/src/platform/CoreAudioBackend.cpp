#include "IAudioBackend.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace AudioRoads::Audio
{
namespace
{

/// @brief 将 CoreFoundation 字符串复制为 UTF-8 值类型。
///
/// 先按最坏情况申请完整缓冲区，转换失败返回空值，不暴露部分编码结果。
/// 函数只借用 CFStringRef，Create/Copy 所有权由外层调用者配平。
/// @param value HAL 属性返回的可空 CoreFoundation 字符串。
/// @return 不含终止符的拥有型 UTF-8 文本；转换失败时为空。
[[nodiscard]] std::string toUtf8(CFStringRef value)
{
    if ( value == nullptr ) return {};
    const auto length = CFStringGetLength(value);
    // 最大编码长度按 UTF-16 code unit 计算并额外预留终止符，避免路径被截断。
    const auto capacity =
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<std::size_t>(capacity), '\0');
    if ( !CFStringGetCString(
             value, result.data(), capacity, kCFStringEncodingUTF8) ) {
        // CoreFoundation 不保证失败缓冲区内容，直接丢弃而不返回半条字符串。
        return {};
    }
    result.resize(std::char_traits<char>::length(result.c_str()));
    return result;
}

/// @brief 读取设备 CFString 属性并立即释放原生对象。
///
/// HAL 字符串属性是可选控制面信息；单项失败返回空字符串，由调用点决定使用
/// UID 或数值 ID 降级。成功取得的 CFString 在 UTF-8 复制后立即释放。
/// @param device 设备或进程 AudioObjectID，别名类型不改变 HAL 调用协议。
/// @param selector 预期返回 CFStringRef 的全局属性选择器。
/// @return 已脱离 CF 对象寿命的 UTF-8 值。
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
        // 名称/UID 的单属性竞态可恢复，调用点以其他稳定信息继续构造快照。
        return {};
    }
    auto result = toUtf8(value);
    // HAL 返回的 CFString 遵循拥有语义，复制为值后立即平衡一次 release。
    if ( value != nullptr ) CFRelease(value);
    // 返回值拥有编码后文本，不再依赖已释放的 CoreFoundation 字符串。
    return result;
}

/// @brief 汇总指定 scope 下所有流的通道数。
///
/// scope 明确区分输入和输出能力。动态 AudioBufferList 仅存活于本次调用，
/// 失败时返回零作为未知/无通道，不把 HAL 指针写入 Core DTO。
/// @param device 当前枚举的 HAL 设备对象。
/// @param scope 输入或输出 scope，不能使用 global 猜测双工能力。
/// @return 所有 AudioBuffer 通道数之和；不支持或读取失败时为零。
[[nodiscard]] std::uint32_t channelCount(AudioDeviceID            device,
                                         AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address{ kAudioDevicePropertyStreamConfiguration,
                                        scope,
                                        kAudioObjectPropertyElementMain };
    UInt32                     size{};
    if ( AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) !=
         noErr ) {
        // scope 不受支持与临时读取失败都按零能力处理，不抛出平台异常。
        return 0;
    }

    // AudioBufferList 长度动态，先查询后一次分配；这里只运行于低频控制面。
    std::vector<std::byte> storage(size);
    // byte 容器只提供满足大小的连续存储，实际布局由 HAL 的 AudioBufferList
    // 定义。
    if ( AudioObjectGetPropertyData(
             device, &address, 0, nullptr, &size, storage.data()) != noErr ) {
        // 尺寸查询与读取之间可能变化，失败时不能解释未完整填充的 buffer list。
        return 0;
    }

    // HAL 保证成功写入时缓冲区采用 AudioBufferList 布局；转换后的观察指针不
    // 逃逸出 storage 寿命，也不承担任何所有权。
    const auto* list = reinterpret_cast<const AudioBufferList*>(storage.data());
    std::uint32_t channels{};
    // 聚合设备可能包含多个 AudioBuffer，通道能力必须求和而不是只读首项。
    for ( UInt32 index = 0; index < list->mNumberBuffers; ++index ) {
        // 只读取通道元数据，不访问 mData 或触碰任何实时音频缓冲区。
        channels += list->mBuffers[index].mNumberChannels;
    }
    return channels;
}

/// @brief 查询系统默认输入或输出设备 ID。
///
/// 选择器由调用方分别传入默认输入和输出属性。读取失败返回 HAL 的 Unknown，
/// 使可选默认标志缺失不会阻断整份设备快照。
/// @param selector 默认输入或默认输出设备属性。
/// @return 当前 AudioDeviceID；读取失败为 kAudioObjectUnknown。
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
        // Unknown 不会与真实默认 ID 命中，等价于“本次快照没有默认信息”。
        return kAudioObjectUnknown;
    }
    return device;
}

/// @brief 枚举当前正在向 CoreAudio 输出的应用进程。
///
/// 进程对象只提供控制面身份，实际数据捕获由 CATapDescription 和 process tap
/// 在启动路由时建立。bundle ID 作为重启稳定键，缺失时退回当前 PID。
/// @warning 依赖 macOS 14.2 process object API，只能在低频控制线程调用。
/// @return 完整应用来源集合；进程列表属性级失败时不返回部分快照。
[[nodiscard]] std::expected<std::vector<Core::AudioSource>, AudioBackendError>
applicationSources()
{
    AudioObjectPropertyAddress address{ kAudioHardwarePropertyProcessObjectList,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    // process object list 属于 system object 全局属性，不应按某个设备 scope
    // 查询。
    UInt32 size{};
    auto   status = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject, &address, 0, nullptr, &size);
    if ( status != noErr ) {
        // 该属性是应用发现的集合边界，失败不能伪装成当前没有正在播放的软件。
        return std::unexpected(AudioBackendError{
            .operation = "读取 CoreAudio 应用进程列表",
            .message   = "OSStatus=" + std::to_string(status) });
    }

    std::vector<AudioObjectID> processObjects(size / sizeof(AudioObjectID));
    // 列表长度来自同一属性的字节数；两次调用之间变化会由第二次 OSStatus 暴露。
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                        &address,
                                        0,
                                        nullptr,
                                        &size,
                                        processObjects.data());
    if ( status != noErr ) {
        return std::unexpected(AudioBackendError{
            .operation = "枚举 CoreAudio 应用进程",
            .message   = "OSStatus=" + std::to_string(status) });
    }

    std::vector<Core::AudioSource> sources;
    sources.reserve(processObjects.size());
    // 每个 process object 都可能在循环期间退出，因此单属性读取失败只跳过该项。
    for ( const auto processObject : processObjects ) {
        AudioObjectPropertyAddress runningAddress{
            kAudioProcessPropertyIsRunningOutput,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 runningOutput{};
        // UInt32 是 HAL 布尔属性的原生存储，不能直接把 C++ bool 地址传给 API。
        UInt32 runningSize = sizeof(runningOutput);
        if ( AudioObjectGetPropertyData(processObject,
                                        &runningAddress,
                                        0,
                                        nullptr,
                                        &runningSize,
                                        &runningOutput) != noErr ||
             runningOutput == 0 ) {
            // 只显示当前实际产生播放数据的软件，避免把所有后台进程塞进画布。
            continue;
        }

        AudioObjectPropertyAddress pidAddress{
            kAudioProcessPropertyPID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        pid_t processId{};
        // pid_t 保持平台 ABI 宽度，复制到 Core 时再验证正值并转为 uint64。
        UInt32 pidSize = sizeof(processId);
        if ( AudioObjectGetPropertyData(processObject,
                                        &pidAddress,
                                        0,
                                        nullptr,
                                        &pidSize,
                                        &processId) != noErr ||
             processId <= 0 || processId == getpid() ) {
            // 排除无效 PID 和本进程，防止未来 process tap 捕获自身形成反馈环。
            continue;
        }

        auto stableId =
            stringProperty(processObject, kAudioProcessPropertyBundleID);
        // 无 bundle ID 的命令行播放器仍可临时路由，但 PID
        // 降级键不能跨重启恢复。
        if ( stableId.empty() ) stableId = std::to_string(processId);
        const auto sourceId = "coreaudio:application:" + stableId;
        const auto duplicate =
            std::ranges::find(sources, sourceId, &Core::AudioSource::id);
        if ( duplicate != sources.end() ) {
            // 同一 bundle 的 helper 进程必须合并进来源，建 tap
            // 时再解析完整集合。
            const auto numericId  = static_cast<std::uint64_t>(processId);
            auto&      processIds = duplicate->application->processIds;
            if ( std::ranges::find(processIds, numericId) ==
                 processIds.end() ) {
                // PID 顺序沿 HAL 列表保持稳定，UI tooltip 不自行排序或去重。
                processIds.push_back(numericId);
            }
            continue;
        }

        sources.push_back(Core::AudioSource{
            .id      = sourceId,
            .name    = stableId,
            .backend = "CoreAudio",
            .kind    = Core::AudioSourceKind::ApplicationOutput,
            // process object 不提供最终 tap 格式，零值要求开流阶段重新协商。
            .channels   = 0,
            .sampleRate = 0,
            .isDefault  = false,
            .application =
                Core::ApplicationIdentity{
                    .processIds = { static_cast<std::uint64_t>(processId) },
                    .stableId   = std::move(stableId) },
        });
    }
    return sources;
}

/// @brief 使用 CoreAudio HAL 发现物理及虚拟音频设备。
///
/// 仅读取控制面属性，不打开 AudioUnit 或 IOProc，也不缓存 AudioDeviceID；后续
/// 建流必须以稳定 UID 重新解析当前原生对象。
/// 枚举期间允许分配 DTO 与属性缓冲区，因此只能从非实时控制面调用。
class CoreAudioBackend final : public IAudioBackend
{
public:
    /// @brief 返回静态后端标签，不触发 HAL 查询。
    [[nodiscard]] const char* name() const noexcept override
    {
        return "CoreAudio";
    }

    /// @brief 同步发现设备与当前运行中的输出进程。
    [[nodiscard]] std::expected<Core::AudioEndpointSnapshot, AudioBackendError>
    enumerateEndpoints() override;
};

std::expected<Core::AudioEndpointSnapshot, AudioBackendError>
CoreAudioBackend::enumerateEndpoints()
{
    // HAL 的列表属性采用“先查询字节数、再读取数组”协议；两步之间设备变化可能
    // 令第二步失败，此时返回整体错误并由 AudioService 保留上一份完整快照。
    AudioObjectPropertyAddress address{ kAudioHardwarePropertyDevices,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    UInt32                     size{};
    auto                       status = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject, &address, 0, nullptr, &size);
    if ( status != noErr ) {
        // OSStatus 数值保留具体系统原因，避免把所有 HAL 故障合并成空设备列表。
        return std::unexpected(AudioBackendError{
            .operation = "读取 CoreAudio 设备数量",
            .message   = "OSStatus=" + std::to_string(status) });
    }

    std::vector<AudioDeviceID> nativeDevices(size / sizeof(AudioDeviceID));
    // 字节数整除原生 ID 大小得到元素数，不在 Core DTO 中保留该临时数组。
    // 设备列表是整体快照；尺寸或数据读取失败时不向上层提交部分集合。
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                        &address,
                                        0,
                                        nullptr,
                                        &size,
                                        nativeDevices.data());
    if ( status != noErr ) {
        // 不返回 nativeDevices 中可能部分写入的数据，维持事务式快照边界。
        return std::unexpected(AudioBackendError{
            .operation = "枚举 CoreAudio 设备",
            .message   = "OSStatus=" + std::to_string(status) });
    }

    const auto defaultInput =
        defaultDevice(kAudioHardwarePropertyDefaultInputDevice);
    const auto defaultOutput =
        defaultDevice(kAudioHardwarePropertyDefaultOutputDevice);
    // 两个默认查询互不依赖，任一失败不会遮蔽另一方向的默认标记。
    // 结果最多与原生列表等长，预留容量避免循环中反复扩容；此处分配不在回调。
    Core::AudioEndpointSnapshot endpoints;
    endpoints.sources.reserve(nativeDevices.size());
    endpoints.targets.reserve(nativeDevices.size());

    for ( const auto nativeDevice : nativeDevices ) {
        // 每项独立读取可选属性，使单个设备的名称/速率故障不会丢失其他设备。
        const auto inputs =
            channelCount(nativeDevice, kAudioObjectPropertyScopeInput);
        const auto outputs =
            channelCount(nativeDevice, kAudioObjectPropertyScopeOutput);
        // HAL 可能列出不具备音频通道的控制对象，这类对象不能成为路由端点。
        if ( inputs == 0 && outputs == 0 ) continue;

        auto uid = stringProperty(nativeDevice, kAudioDevicePropertyDeviceUID);
        // UID 是跨枚举的首选稳定键；缺失时数值 ID 只作为本次会话降级标识。
        if ( uid.empty() ) uid = std::to_string(nativeDevice);
        auto name = stringProperty(nativeDevice, kAudioObjectPropertyName);
        // 名称是展示信息，缺失时用稳定键保证组合框与路由列表仍有可识别文本。
        if ( name.empty() ) name = uid;

        Float64 sampleRate{};
        // 可选标称速率使用独立属性查询，其失败不会否定已经确认的通道能力。
        AudioObjectPropertyAddress rateAddress{
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 rateSize = sizeof(sampleRate);
        AudioObjectGetPropertyData(
            nativeDevice, &rateAddress, 0, nullptr, &rateSize, &sampleRate);
        // 标称采样率读取失败保留零值；真正打开流时仍需重新确认和协商格式。

        // 双工 HAL 设备拆为同一 UID 下的两个路由端点，Core 不再承担平台方向
        // 推断。DTO 只保存值，AudioDeviceID 和 CF 对象均不跨层泄漏。
        if ( inputs > 0 ) {
            // 同一 UID 添加 device-input 前缀后成为可持久化来源 ID。
            endpoints.sources.push_back(Core::AudioSource{
                .id          = "coreaudio:device-input:" + uid,
                .name        = name,
                .backend     = "CoreAudio",
                .kind        = Core::AudioSourceKind::DeviceInput,
                .channels    = inputs,
                .sampleRate  = static_cast<std::uint32_t>(sampleRate),
                .isDefault   = nativeDevice == defaultInput,
                .application = std::nullopt,
            });
        }
        if ( outputs > 0 ) {
            // name 在最后一个方向移动；双工设备的输入 DTO 已先完成自己的复制。
            endpoints.targets.push_back(Core::AudioTarget{
                .id         = "coreaudio:device-output:" + uid,
                .name       = std::move(name),
                .backend    = "CoreAudio",
                .kind       = Core::AudioTargetKind::DeviceOutput,
                .channels   = outputs,
                .sampleRate = static_cast<std::uint32_t>(sampleRate),
                .isDefault  = nativeDevice == defaultOutput,
            });
        }
    }
    // 应用来源与物理输入属于同一 sources 列，但单独枚举以保持 HAL
    // 对象语义清晰。
    auto applications = applicationSources();
    // process 列表是完整快照的一部分，失败时不能悄悄只返回设备造成 UI 误判。
    if ( !applications )
        return std::unexpected(std::move(applications.error()));
    // move_iterator 转移每个拥有型字符串与 PID 容器，不复制控制面快照。
    endpoints.sources.insert(endpoints.sources.end(),
                             std::make_move_iterator(applications->begin()),
                             std::make_move_iterator(applications->end()));
    return endpoints;
}

}  // namespace

std::unique_ptr<IAudioBackend> createCoreAudioBackend()
{
    // 平台类保持翻译单元私有，避免 CoreAudio 头或句柄进入公共接口。
    return std::make_unique<CoreAudioBackend>();
}

}  // namespace AudioRoads::Audio
