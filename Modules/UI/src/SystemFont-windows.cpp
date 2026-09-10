#include "SystemFont.h"

#define NOMINMAX
#include <Windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace AudioRoads::UI
{
namespace
{

using Microsoft::WRL::ComPtr;

/// @brief 把 Windows UTF-16 文件路径转换为 UTF-8。
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

/// @brief 用 DirectWrite 将字体族解析到本地字体文件和 TTC 索引。
[[nodiscard]] std::expected<SystemFontFace, std::string> resolveFamily(
    IDWriteFactory& factory, const wchar_t* familyName,
    DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style)
{
    ComPtr<IDWriteFontCollection> collection;
    if ( FAILED(factory.GetSystemFontCollection(&collection, FALSE)) ) {
        return std::unexpected("无法读取 DirectWrite 系统字体集合");
    }

    UINT32 familyIndex{};
    BOOL   familyExists{};
    if ( FAILED(collection->FindFamilyName(
             familyName, &familyIndex, &familyExists)) ||
         familyExists == FALSE ) {
        return std::unexpected("系统字体族不存在");
    }

    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteFont>       font;
    ComPtr<IDWriteFontFace>   face;
    if ( FAILED(collection->GetFontFamily(familyIndex, &family)) ||
         FAILED(family->GetFirstMatchingFont(
             weight, DWRITE_FONT_STRETCH_NORMAL, style, &font)) ||
         FAILED(font->CreateFontFace(&face)) ) {
        return std::unexpected("无法创建 DirectWrite 字形面");
    }

    // 常规桌面 UI 字体通常只引用一个本地文件；多文件复合字体交给后续回退族。
    UINT32           fileCount = 1;
    IDWriteFontFile* nativeFile{};
    if ( FAILED(face->GetFiles(&fileCount, &nativeFile)) ||
         nativeFile == nullptr ) {
        return std::unexpected("DirectWrite 字形面没有单一字体文件");
    }
    ComPtr<IDWriteFontFile> file;
    file.Attach(nativeFile);

    const void*                        referenceKey{};
    UINT32                             referenceKeySize{};
    ComPtr<IDWriteFontFileLoader>      loader;
    ComPtr<IDWriteLocalFontFileLoader> localLoader;
    if ( FAILED(file->GetReferenceKey(&referenceKey, &referenceKeySize)) ||
         FAILED(file->GetLoader(&loader)) || FAILED(loader.As(&localLoader)) ) {
        return std::unexpected("DirectWrite 字体不是本地文件");
    }

    UINT32 pathLength{};
    if ( FAILED(localLoader->GetFilePathLengthFromKey(
             referenceKey, referenceKeySize, &pathLength)) ) {
        return std::unexpected("无法读取 DirectWrite 字体路径长度");
    }
    std::wstring path(static_cast<std::size_t>(pathLength) + 1U, L'\0');
    if ( FAILED(localLoader->GetFilePathFromKey(
             referenceKey, referenceKeySize, path.data(), pathLength + 1U)) ) {
        return std::unexpected("无法读取 DirectWrite 字体路径");
    }

    auto utf8Path = toUtf8(path.c_str());
    if ( utf8Path.empty() ) {
        return std::unexpected("DirectWrite 字体路径无法转换为 UTF-8");
    }
    return SystemFontFace{ .path      = std::move(utf8Path),
                           .faceIndex = face->GetIndex() };
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
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(NONCLIENTMETRICSW);
    if ( SystemParametersInfoW(
             SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) == FALSE ) {
        return std::unexpected("无法读取 Windows 系统 UI 字体");
    }

    ComPtr<IDWriteFactory> factory;
    const auto             factoryResult = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    if ( FAILED(factoryResult) ) {
        return std::unexpected("无法创建 DirectWrite factory");
    }

    const auto weight = static_cast<DWRITE_FONT_WEIGHT>(
        std::clamp(metrics.lfMessageFont.lfWeight,
                   static_cast<LONG>(DWRITE_FONT_WEIGHT_THIN),
                   static_cast<LONG>(DWRITE_FONT_WEIGHT_ULTRA_BLACK)));
    const auto style   = metrics.lfMessageFont.lfItalic != FALSE
                             ? DWRITE_FONT_STYLE_ITALIC
                             : DWRITE_FONT_STYLE_NORMAL;
    auto       primary = resolveFamily(
        *factory.Get(), metrics.lfMessageFont.lfFaceName, weight, style);
    if ( !primary ) {
        primary = resolveFamily(*factory.Get(),
                                L"Segoe UI",
                                DWRITE_FONT_WEIGHT_NORMAL,
                                DWRITE_FONT_STYLE_NORMAL);
    }
    if ( !primary ) return std::unexpected(std::move(primary.error()));

    std::vector<SystemFontFace> faces;
    faces.reserve(4);
    appendUnique(faces, std::move(*primary));

    // DirectWrite 的平台级 fallback 不参与 ImGui 的 stb_truetype 渲染，因此把
    // Windows 常见系统 CJK 与 Emoji UI 字体作为缺字回退源合并。
    constexpr std::array FALLBACK_FAMILIES{ L"Microsoft YaHei UI",
                                            L"Yu Gothic UI",
                                            L"Malgun Gothic",
                                            L"Segoe UI Emoji" };
    for ( const auto* family : FALLBACK_FAMILIES ) {
        auto fallback = resolveFamily(*factory.Get(),
                                      family,
                                      DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL);
        if ( fallback ) appendUnique(faces, std::move(*fallback));
    }
    return faces;
}

}  // namespace AudioRoads::UI
