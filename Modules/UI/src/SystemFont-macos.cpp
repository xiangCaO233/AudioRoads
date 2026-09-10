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

/// @brief CoreFoundation 对象的局部 RAII 所有者。
template<typename T> class CfOwner final
{
public:
    /// @brief 接管带有 Create/Copy 所有权的对象。
    explicit CfOwner(T value) : m_value(value) {}

    /// @brief 释放已接管的 CoreFoundation 对象。
    ~CfOwner()
    {
        if ( m_value != nullptr ) CFRelease(m_value);
    }

    CfOwner(const CfOwner&)            = delete;
    CfOwner& operator=(const CfOwner&) = delete;

    /// @brief 返回不转移所有权的观察句柄。
    [[nodiscard]] T get() const noexcept { return m_value; }

private:
    /// @brief 当前拥有的 CoreFoundation 句柄。
    T m_value{};
};

/// @brief 在 TTF/OTF 或 TTC/OTC 文件中定位 CoreText 选中的字形面。
[[nodiscard]] std::uint32_t faceIndexForFont(CTFontRef font, CFURLRef url)
{
    CfOwner<CFStringRef> postScriptName{ CTFontCopyPostScriptName(font) };
    CfOwner<CFArrayRef>  descriptors{ CTFontManagerCreateFontDescriptorsFromURL(
        url) };
    if ( postScriptName.get() == nullptr || descriptors.get() == nullptr ) {
        return 0;
    }

    const auto count = CFArrayGetCount(descriptors.get());
    for ( CFIndex index{}; index < count; ++index ) {
        const auto descriptor = static_cast<CTFontDescriptorRef>(
            CFArrayGetValueAtIndex(descriptors.get(), index));
        if ( descriptor == nullptr ) continue;

        CfOwner<CFTypeRef> candidateName{ CTFontDescriptorCopyAttribute(
            descriptor, kCTFontNameAttribute) };
        if ( candidateName.get() != nullptr &&
             CFGetTypeID(candidateName.get()) == CFStringGetTypeID() &&
             CFEqual(candidateName.get(), postScriptName.get()) ) {
            return static_cast<std::uint32_t>(index);
        }
    }
    return 0;
}

/// @brief 从 CoreText 字形面复制字体文件路径和集合索引。
[[nodiscard]] std::expected<SystemFontFace, std::string> faceFromFont(
    CTFontRef font)
{
    CfOwner<CFTypeRef> urlValue{ CTFontCopyAttribute(font,
                                                     kCTFontURLAttribute) };
    if ( urlValue.get() == nullptr ||
         CFGetTypeID(urlValue.get()) != CFURLGetTypeID() ) {
        return std::unexpected("CoreText 系统字体缺少文件 URL");
    }

    std::array<UInt8, PATH_MAX> path{};
    if ( CFURLGetFileSystemRepresentation(static_cast<CFURLRef>(urlValue.get()),
                                          true,
                                          path.data(),
                                          static_cast<CFIndex>(path.size())) ==
         false ) {
        return std::unexpected("无法读取 CoreText 字体文件路径");
    }

    const auto url = static_cast<CFURLRef>(urlValue.get());
    return SystemFontFace{
        .path      = reinterpret_cast<const char*>(path.data()),
        .faceIndex = faceIndexForFont(font, url),
    };
}

/// @brief 只追加尚未存在的字体文件和字形面组合。
void appendUnique(std::vector<SystemFontFace>& faces, SystemFontFace face)
{
    const auto duplicate =
        std::ranges::find_if(faces, [&](const SystemFontFace& existing) {
            return existing.path == face.path &&
                   existing.faceIndex == face.faceIndex;
        });
    if ( duplicate == faces.end() ) faces.push_back(std::move(face));
}

}  // namespace

std::expected<std::vector<SystemFontFace>, std::string> resolveSystemFontFaces()
{
    CfOwner<CTFontRef> primary{ CTFontCreateUIFontForLanguage(
        kCTFontUIFontSystem, 0.0, nullptr) };
    if ( primary.get() == nullptr ) {
        return std::unexpected("CoreText 没有返回系统 UI 字体");
    }

    auto primaryFace = faceFromFont(primary.get());
    if ( !primaryFace ) {
        return std::unexpected(std::move(primaryFace.error()));
    }

    std::vector<SystemFontFace> faces;
    faces.reserve(4);
    appendUnique(faces, std::move(*primaryFace));

    // 逐个用真实 CJK 文字询问 CoreText cascade，避免硬编码
    // PingFang 等版本相关路径，也避免一次匹配只返回首个缺字字体。
    constexpr std::array<UniChar, 3> CJK_SAMPLE{ 0x6C49, 0x304B, 0xD55C };
    for ( const auto character : CJK_SAMPLE ) {
        CfOwner<CFStringRef> sample{ CFStringCreateWithCharacters(
            kCFAllocatorDefault, &character, 1) };
        if ( sample.get() == nullptr ) continue;

        CfOwner<CTFontRef> fallback{ CTFontCreateForString(
            primary.get(), sample.get(), CFRangeMake(0, 1)) };
        if ( fallback.get() != nullptr ) {
            auto fallbackFace = faceFromFont(fallback.get());
            if ( fallbackFace ) appendUnique(faces, std::move(*fallbackFace));
        }
    }
    return faces;
}

}  // namespace AudioRoads::UI
