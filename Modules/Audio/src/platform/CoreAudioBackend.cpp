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
///
/// 先按最坏情况申请完整缓冲区，转换失败返回空值，不暴露部分编码结果。
/// 函数只借用 CFStringRef，Create/Copy 所有权由外层调用者配平。
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

/// @brief 使用 CoreAudio HAL 发现物理及虚拟音频设备。
///
/// 仅读取控制面属性，不打开 AudioUnit 或 IOProc，也不缓存 AudioDeviceID；后续
/// 建流必须以稳定 UID 重新解析当前原生对象。
/// 枚举期间允许分配 DTO 与属性缓冲区，因此只能从非实时控制面调用。
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
    std::vector<Core::AudioDevice> devices;
    devices.reserve(nativeDevices.size());

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

        // 能力由两个 scope 的实际通道汇总决定，不能仅依赖设备名称或 transport。
        const auto flow = inputs > 0 && outputs > 0
                              ? Core::DeviceFlow::Duplex
                              : (inputs > 0 ? Core::DeviceFlow::Input
                                            : Core::DeviceFlow::Output);
        // DTO 只保存值；CF 对象、property address 和 AudioDeviceID
        // 均不跨层泄漏。
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
    // 默认输入或输出任一命中即标记默认，与跨平台“常用端点”展示语义一致。
    return devices;
}

}  // namespace

std::unique_ptr<IAudioBackend> createCoreAudioBackend()
{
    // 平台类保持翻译单元私有，避免 CoreAudio 头或句柄进入公共接口。
    return std::make_unique<CoreAudioBackend>();
}

}  // namespace AudioRoads::Audio
