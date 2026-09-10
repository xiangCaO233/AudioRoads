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

/// @brief Fontconfig pattern 的无状态销毁器。
///
/// 配合 unique_ptr 覆盖 matchFont 的所有早退分支；空指针检查允许默认构造和
/// 未匹配状态复用同一所有者类型。
struct PatternDeleter {
    /// @brief 释放 Fontconfig pattern。
    void operator()(FcPattern* pattern) const noexcept
    {
        if ( pattern != nullptr ) FcPatternDestroy(pattern);
    }
};

/// @brief Fontconfig 配置快照的无状态销毁器。
///
/// resolveSystemFontFaces 独占 FcInitLoadConfigAndFonts 返回值，不借用全局
/// 配置，因此结束查询后可确定性释放其中字体数据库资源。
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
///
/// 通用族固定为 sans-serif，但具体字形面由当前用户配置、桌面别名和语言规则
/// 决定。返回结果立即复制 FC_FILE 与 FC_INDEX，不把 matched pattern 泄漏出去。
/// @param language 空值表示主 UI 字体，非空值用于约束缺字回退语言。
[[nodiscard]] std::expected<SystemFontFace, std::string> matchFont(
    FcConfig& config, std::string_view language)
{
    // pattern 和最终匹配都用独立所有者，任意错误返回都会走 Fontconfig 销毁器。
    PatternOwner pattern{ FcPatternCreate() };
    // 所有者先建立再配置，后续任一步失败都不会泄漏半成品 pattern。
    if ( !pattern ) return std::unexpected("无法创建 Fontconfig pattern");

    const auto* SANS_SERIF = reinterpret_cast<const FcChar8*>("sans-serif");
    // 不硬编码发行版字体族，sans-serif 别名才能服从 GNOME、KDE 和用户规则。
    if ( FcPatternAddString(pattern.get(), FC_FAMILY, SANS_SERIF) != FcTrue ) {
        // 族条件是有效查询的最低要求，写入失败不能继续做无约束匹配。
        return std::unexpected("无法设置 Fontconfig 字体族");
    }
    if ( !language.empty() ) {
        // 语言只约束 CJK 回退；主字体留空以遵循桌面 sans-serif 用户偏好。
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
    // default substitute 补齐样式、像素尺寸等缺省属性，但不覆盖用户配置结果。

    FcResult     result{};
    PatternOwner matched{ FcFontMatch(&config, pattern.get(), &result) };
    // 匹配对象仍由本函数拥有，后续只提取文件路径与集合索引两个值字段。
    if ( !matched ) return std::unexpected("Fontconfig 没有匹配到字体");
    // result 仅供 Fontconfig 描述匹配状态，是否可用仍以 matched 和 FC_FILE
    // 为准。

    FcChar8* nativePath{};
    if ( FcPatternGetString(matched.get(), FC_FILE, 0, &nativePath) !=
             FcResultMatch ||
         nativePath == nullptr ) {
        return std::unexpected("Fontconfig 匹配结果缺少字体路径");
    }
    // FC_FILE 指针依附 matched pattern，构造 std::string 后才能跨出当前作用域。

    int faceIndex{};
    if ( FcPatternGetInteger(matched.get(), FC_INDEX, 0, &faceIndex) !=
         FcResultMatch ) {
        // 普通 TTF/OTF 没有 FC_INDEX；缺失或负值均按首个字形面处理。
        faceIndex = 0;
    }
    return SystemFontFace{
        // nativePath 在此复制成 std::string，matched 随后销毁不会影响返回值。
        .path      = reinterpret_cast<const char*>(nativePath),
        .faceIndex = faceIndex < 0 ? 0U : static_cast<std::uint32_t>(faceIndex),
    };
}

/// @brief 只追加尚未存在的字体文件和字形面组合。
///
/// 同一 TTC/OTC 路径的不同面必须视为不同字体；完全相同的组合则避免被多个
/// CJK 语言规则重复合并进 ImGui 字体图集。
void appendUnique(std::vector<SystemFontFace>& faces, SystemFontFace face)
{
    const auto duplicate =
        std::ranges::find_if(faces, [&](const SystemFontFace& existing) {
            return existing.path == face.path &&
                   existing.faceIndex == face.faceIndex;
        });
    // 比较发生在 move 前，face 的路径和索引仍完整可读。
    // 只有确认为新组合才移动 face，查询阶段不制造重复图集输入。
    if ( duplicate == faces.end() ) faces.push_back(std::move(face));
}

}  // namespace

std::expected<std::vector<SystemFontFace>, std::string> resolveSystemFontFaces()
{
    // 独立配置快照应用当前用户与桌面规则，返回后不留下 Fontconfig 借用指针。
    ConfigOwner config{ FcInitLoadConfigAndFonts() };
    // 配置加载包含系统与用户字体数据库，只在启动阶段执行，不能进入逐帧路径。
    if ( !config ) return std::unexpected("无法初始化 Fontconfig");
    // 无配置意味着无法可靠取得用户偏好，不能硬编码某个发行版字体路径。

    auto primary = matchFont(*config, {});
    // 主字体是可用界面的必要条件；它失败时交由 AppWindow 使用内建字体降级。
    if ( !primary ) return std::unexpected(std::move(primary.error()));

    std::vector<SystemFontFace> faces;
    // 一项主字体加三种 CJK 回退，预留容量避免小 vector 反复扩容。
    faces.reserve(4);
    appendUnique(faces, std::move(*primary));
    // 主字体必须保持首项，AppWindow 以此决定 MergeMode 的初始状态。

    // ImGui 使用字体文件而不是平台排版器，需显式合并系统为各语言选择的回退。
    for ( const auto language : { "zh-cn", "ja", "ko" } ) {
        auto fallback = matchFont(*config, language);
        // 回退是尽力而为：某语言包缺失不应否定已成功的主 UI 字体。
        if ( fallback ) appendUnique(faces, std::move(*fallback));
    }
    return faces;
}

}  // namespace AudioRoads::UI
