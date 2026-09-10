#include "SystemFont.h"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace AudioRoads::UI
{
namespace
{

/// @brief Fontconfig 对象销毁器，确保失败分支不会泄漏 pattern。
struct PatternDeleter {
    /// @brief 释放 Fontconfig pattern。
    void operator()(FcPattern* pattern) const noexcept
    {
        if ( pattern != nullptr ) FcPatternDestroy(pattern);
    }
};

/// @brief Fontconfig 配置销毁器。
struct ConfigDeleter {
    /// @brief 释放当前解析使用的独立配置快照。
    void operator()(FcConfig* config) const noexcept
    {
        if ( config != nullptr ) FcConfigDestroy(config);
    }
};

using PatternOwner = std::unique_ptr<FcPattern, PatternDeleter>;
using ConfigOwner  = std::unique_ptr<FcConfig, ConfigDeleter>;

/// @brief 根据通用族和可选语言执行一次完整 Fontconfig 匹配。
[[nodiscard]] std::expected<SystemFontFace, std::string> matchFont(
    FcConfig& config, std::string_view language)
{
    PatternOwner pattern{ FcPatternCreate() };
    if ( !pattern ) return std::unexpected("无法创建 Fontconfig pattern");

    const auto* SANS_SERIF = reinterpret_cast<const FcChar8*>("sans-serif");
    if ( FcPatternAddString(pattern.get(), FC_FAMILY, SANS_SERIF) != FcTrue ) {
        return std::unexpected("无法设置 Fontconfig 字体族");
    }
    if ( !language.empty() ) {
        const std::string languageValue{ language };
        const auto*       value =
            reinterpret_cast<const FcChar8*>(languageValue.c_str());
        if ( FcPatternAddString(pattern.get(), FC_LANG, value) != FcTrue ) {
            return std::unexpected("无法设置 Fontconfig 字体语言");
        }
    }

    // substitute 与 default substitute 必须在 FcFontMatch 之前执行，否则用户和
    // 桌面环境配置不会参与最终匹配。
    if ( FcConfigSubstitute(&config, pattern.get(), FcMatchPattern) !=
         FcTrue ) {
        return std::unexpected("Fontconfig 用户配置替换失败");
    }
    FcDefaultSubstitute(pattern.get());

    FcResult     result{};
    PatternOwner matched{ FcFontMatch(&config, pattern.get(), &result) };
    if ( !matched ) return std::unexpected("Fontconfig 没有匹配到字体");

    FcChar8* nativePath{};
    if ( FcPatternGetString(matched.get(), FC_FILE, 0, &nativePath) !=
             FcResultMatch ||
         nativePath == nullptr ) {
        return std::unexpected("Fontconfig 匹配结果缺少字体路径");
    }

    int faceIndex{};
    if ( FcPatternGetInteger(matched.get(), FC_INDEX, 0, &faceIndex) !=
         FcResultMatch ) {
        faceIndex = 0;
    }
    return SystemFontFace{
        .path      = reinterpret_cast<const char*>(nativePath),
        .faceIndex = faceIndex < 0 ? 0U : static_cast<std::uint32_t>(faceIndex),
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
    ConfigOwner config{ FcInitLoadConfigAndFonts() };
    if ( !config ) return std::unexpected("无法初始化 Fontconfig");

    auto primary = matchFont(*config, {});
    if ( !primary ) return std::unexpected(std::move(primary.error()));

    std::vector<SystemFontFace> faces;
    faces.reserve(4);
    appendUnique(faces, std::move(*primary));

    // ImGui 使用字体文件而不是平台排版器，需显式合并系统为各语言选择的回退。
    for ( const auto language : { "zh-cn", "ja", "ko" } ) {
        auto fallback = matchFont(*config, language);
        if ( fallback ) appendUnique(faces, std::move(*fallback));
    }
    return faces;
}

}  // namespace AudioRoads::UI
