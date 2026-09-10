#include "SystemFont.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <limits.h>
#include <string>
#include <utility>
#include <vector>

namespace AudioRoads::UI
{
namespace
{

/// @brief CoreFoundation Create/Copy 对象的局部 RAII 所有者。
/// @note 禁止复制，确保同一 CFTypeRef 只被释放一次。
template<typename T> class CfOwner final
{
public:
    /// @brief 接管带有 Create/Copy 所有权的对象。
    explicit CfOwner(T value) : m_value(value) {}

    /// @brief 释放已接管的 CoreFoundation 对象。
    ~CfOwner()
    {
        // CFRelease 接受所有具体 CFTypeRef；空句柄表示 Create/Copy 已失败。
        if ( m_value != nullptr ) CFRelease(m_value);
    }

    CfOwner(const CfOwner&)            = delete;
    CfOwner& operator=(const CfOwner&) = delete;
    // 当前用途不转移句柄，故意不提供移动，避免模板实例间模糊所有权交接。

    /// @brief 返回不转移所有权的观察句柄。
    [[nodiscard]] T get() const noexcept { return m_value; }

private:
    /// @brief 当前独占且将在析构时 CFRelease 的 CoreFoundation 句柄。
    T m_value{};
};

/// @brief 在 TTF/OTF 或 TTC/OTC 文件中定位 CoreText 选中的字形面。
///
/// CoreText 只给出字体引用与共享文件 URL，而 ImGui 还需要集合中的数值索引。
/// 描述符顺序按文件中字形面排列，以 PostScript 名称匹配可避免本地化族名歧义。
[[nodiscard]] std::uint32_t faceIndexForFont(CTFontRef font, CFURLRef url)
{
    // TTC/OTC 共享文件路径，需用 PostScript 名称在描述符顺序中找真实面索引。
    CfOwner<CFStringRef> postScriptName{ CTFontCopyPostScriptName(font) };
    CfOwner<CFArrayRef>  descriptors{ CTFontManagerCreateFontDescriptorsFromURL(
        url) };
    if ( postScriptName.get() == nullptr || descriptors.get() == nullptr ) {
        // 普通单面字体或不可枚举集合安全降级到第零面。
        return 0;
    }

    const auto count = CFArrayGetCount(descriptors.get());
    // 描述符数组只在本函数内借用，任何候选属性都用独立 CfOwner 接管。
    for ( CFIndex index{}; index < count; ++index ) {
        // CFArrayGetValueAtIndex 返回借用对象，不可单独 release
        // 或跨集合寿命保存。
        const auto descriptor = static_cast<CTFontDescriptorRef>(
            CFArrayGetValueAtIndex(descriptors.get(), index));
        if ( descriptor == nullptr ) continue;
        // 空描述符属于局部异常，继续检查集合中的其他字形面。

        CfOwner<CFTypeRef> candidateName{ CTFontDescriptorCopyAttribute(
            descriptor, kCTFontNameAttribute) };
        if ( candidateName.get() != nullptr &&
             CFGetTypeID(candidateName.get()) == CFStringGetTypeID() &&
             CFEqual(candidateName.get(), postScriptName.get()) ) {
            // 数组顺序即 TTC/OTC face index，匹配后即可结束额外属性查询。
            return static_cast<std::uint32_t>(index);
        }
    }
    return 0;
}

/// @brief 从 CoreText 字形面复制字体文件路径和集合索引。
///
/// 返回对象完全拥有 UTF-8 路径，且不携带 CFURL/CTFont 句柄。无法映射到本地
/// 文件的字体不能交给 ImGui 的文件加载 API，必须返回显式错误。
[[nodiscard]] std::expected<SystemFontFace, std::string> faceFromFont(
    CTFontRef font)
{
    // ImGui 只能从本地文件加载，非文件字体即使 CoreText 可渲染也不能返回。
    CfOwner<CFTypeRef> urlValue{ CTFontCopyAttribute(font,
                                                     kCTFontURLAttribute) };
    if ( urlValue.get() == nullptr ||
         CFGetTypeID(urlValue.get()) != CFURLGetTypeID() ) {
        // 类型检查先于 cast，防止损坏或非标准属性值被解释成 CFURL。
        return std::unexpected("CoreText 系统字体缺少文件 URL");
    }

    std::array<UInt8, PATH_MAX> path{};
    // CoreFoundation 直接写入 UTF-8 文件系统表示，离开函数前复制为
    // std::string。
    if ( CFURLGetFileSystemRepresentation(static_cast<CFURLRef>(urlValue.get()),
                                          true,
                                          path.data(),
                                          static_cast<CFIndex>(path.size())) ==
         false ) {
        return std::unexpected("无法读取 CoreText 字体文件路径");
    }

    const auto url = static_cast<CFURLRef>(urlValue.get());
    // 在 urlValue RAII 所有者离开前完成集合索引查询和路径值复制。
    return SystemFontFace{
        // path 缓冲在本地栈上，聚合初始化会在离开函数前复制为 std::string。
        .path      = reinterpret_cast<const char*>(path.data()),
        .faceIndex = faceIndexForFont(font, url),
    };
}

/// @brief 只追加尚未存在的字体文件和字形面组合。
///
/// 不同 CJK 样本可能由同一系统 fallback 覆盖，按路径与面索引去重可避免
/// ImGui 重复合并；同文件的不同集合面仍保持独立。
void appendUnique(std::vector<SystemFontFace>& faces, SystemFontFace face)
{
    const auto duplicate =
        std::ranges::find_if(faces, [&](const SystemFontFace& existing) {
            return existing.path == face.path &&
                   existing.faceIndex == face.faceIndex;
        });
    // 只有不重复时才移动 face，保证比较期间候选值未进入 moved-from 状态。
    if ( duplicate == faces.end() ) faces.push_back(std::move(face));
}

}  // namespace

std::expected<std::vector<SystemFontFace>, std::string> resolveSystemFontFaces()
{
    // 使用系统 UI 字体与当前语言；字号和 DPI 由 AppWindow 统一管理。
    CfOwner<CTFontRef> primary{ CTFontCreateUIFontForLanguage(
        kCTFontUIFontSystem, 0.0, nullptr) };
    // 0.0 请求系统建议 UI 尺寸，但这里只消费字形面；实际字号由 ImGui 管理。
    if ( primary.get() == nullptr ) {
        return std::unexpected("CoreText 没有返回系统 UI 字体");
    }

    auto primaryFace = faceFromFont(primary.get());
    // 主字体若不是本地文件则无法供 ImGui 使用，交由上层统一选择内建字体。
    if ( !primaryFace ) {
        return std::unexpected(std::move(primaryFace.error()));
    }

    std::vector<SystemFontFace> faces;
    // 主字体与三个字符级回退构成小集合，预留容量避免追加时重复分配。
    faces.reserve(4);
    appendUnique(faces, std::move(*primaryFace));
    // 主系统字体必须先进入列表，后续 cascade 只能作为 MergeMode 回退。

    // 逐个用真实 CJK 文字询问 CoreText cascade，避免硬编码
    // PingFang 等版本相关路径，也避免一次匹配只返回首个缺字字体。
    constexpr std::array<UniChar, 3> CJK_SAMPLE{ 0x6C49, 0x304B, 0xD55C };
    for ( const auto character : CJK_SAMPLE ) {
        // 每次只查询一个脚本代表字符，避免 range API 只给出首个缺字 fallback。
        CfOwner<CFStringRef> sample{ CFStringCreateWithCharacters(
            kCFAllocatorDefault, &character, 1) };
        if ( sample.get() == nullptr ) continue;
        // 样本文本创建失败只跳过该脚本，已解析字体列表仍保持可用。

        CfOwner<CTFontRef> fallback{ CTFontCreateForString(
            primary.get(), sample.get(), CFRangeMake(0, 1)) };
        if ( fallback.get() != nullptr ) {
            // 单个字符的 cascade 结果可能仍是主字体，appendUnique 会自然去重。
            auto fallbackFace = faceFromFont(fallback.get());
            // 某个语言的字体不可映射为文件时跳过，不影响已解析的其他字体。
            if ( fallbackFace ) appendUnique(faces, std::move(*fallbackFace));
        }
    }
    return faces;
}

}  // namespace AudioRoads::UI
