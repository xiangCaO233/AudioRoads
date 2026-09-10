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

/// @brief 把 Windows UTF-16 文件路径完整转换为 UTF-8。
///
/// 第一遍查询包含终止符的容量，第二遍写入；失败或只有空终止符时返回空值，
/// 防止将截断路径交给 ImGui 文件加载器。
[[nodiscard]] std::string toUtf8(const wchar_t* value)
{
    if ( value == nullptr || *value == L'\0' ) return {};
    const auto size = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    // 长度查询失败或只有终止符时不执行第二次转换。
    if ( size <= 1 ) return {};
    // 只有容量有效才分配目标字符串，转换逻辑不使用显式 new/delete。
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

/// @brief 用 DirectWrite 将字体族解析到本地字体文件和 TTC 索引。
///
/// 族、字重和样式先解析为具体 face，再要求其 loader 支持本地路径。远程或
/// 复合非本地字体即使可由 DirectWrite 绘制，也不能交给 ImGui 文件接口。
/// 所有 COM 接口由 ComPtr 保持到 referenceKey 路径复制结束。
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
    // “调用成功但族不存在”与 HRESULT 失败都不能继续使用未定义 familyIndex。
    if ( FAILED(collection->FindFamilyName(
             familyName, &familyIndex, &familyExists)) ||
         familyExists == FALSE ) {
        // 缺失族是可恢复结果，调用方可尝试 Segoe UI 或下一回退族。
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
    // GetFirstMatchingFont 使用最接近的合法 face，保留系统设置的字重和斜体。

    // 常规桌面 UI 字体通常只引用一个本地文件；多文件复合字体交给后续回退族。
    UINT32           fileCount = 1;
    IDWriteFontFile* nativeFile{};
    if ( FAILED(face->GetFiles(&fileCount, &nativeFile)) ||
         nativeFile == nullptr ) {
        // ImGui 当前入口只接受单一本地文件，复合 face 留给显式回退族处理。
        return std::unexpected("DirectWrite 字形面没有单一字体文件");
    }
    ComPtr<IDWriteFontFile> file;
    // GetFiles 返回调用方拥有的一次引用，Attach 接管它且不额外 AddRef。
    file.Attach(nativeFile);

    const void*                        referenceKey{};
    UINT32                             referenceKeySize{};
    ComPtr<IDWriteFontFileLoader>      loader;
    ComPtr<IDWriteLocalFontFileLoader> localLoader;
    if ( FAILED(file->GetReferenceKey(&referenceKey, &referenceKeySize)) ||
         FAILED(file->GetLoader(&loader)) || FAILED(loader.As(&localLoader)) ) {
        return std::unexpected("DirectWrite 字体不是本地文件");
    }

    // referenceKey 只在 file 与 loader
    // 存活期间有效，必须在本作用域完成路径复制。
    UINT32 pathLength{};
    if ( FAILED(localLoader->GetFilePathLengthFromKey(
             referenceKey, referenceKeySize, &pathLength)) ) {
        // 路径长度失败时不能猜测 MAX_PATH，现代字体位置可能使用长路径。
        return std::unexpected("无法读取 DirectWrite 字体路径长度");
    }
    std::wstring path(static_cast<std::size_t>(pathLength) + 1U, L'\0');
    // DirectWrite 长度不含终止符，因此容器和 API 容量都显式加一。
    if ( FAILED(localLoader->GetFilePathFromKey(
             referenceKey, referenceKeySize, path.data(), pathLength + 1U)) ) {
        return std::unexpected("无法读取 DirectWrite 字体路径");
    }

    auto utf8Path = toUtf8(path.c_str());
    if ( utf8Path.empty() ) {
        return std::unexpected("DirectWrite 字体路径无法转换为 UTF-8");
    }
    return SystemFontFace{
        .path = std::move(utf8Path),
        // 保留 TTC/OTC 真实字形面，避免 ImGui 加载同文件的错误字体。
        .faceIndex = face->GetIndex()
    };
}

/// @brief 只追加尚未存在的字体文件和字形面组合。
///
/// Windows 多个 UI 族可能最终指向同一文件与 face；去重避免 ImGui 重复合并。
/// 同 TTC 文件的不同 faceIndex 仍是独立候选，不能只按路径判断。
void appendUnique(std::vector<SystemFontFace>& faces, SystemFontFace face)
{
    const auto duplicate =
        std::ranges::find_if(faces, [&](const SystemFontFace& existing) {
            return existing.path == face.path &&
                   existing.faceIndex == face.faceIndex;
        });
    // 候选只在确认唯一后移动，避免比较 moved-from 路径导致重复项漏判。
    if ( duplicate == faces.end() ) faces.push_back(std::move(face));
}

}  // namespace

std::expected<std::vector<SystemFontFace>, std::string> resolveSystemFontFaces()
{
    NONCLIENTMETRICSW metrics{};
    // cbSize 是 SystemParametersInfoW 识别结构版本的必要输入，必须先初始化。
    metrics.cbSize = sizeof(NONCLIENTMETRICSW);
    if ( SystemParametersInfoW(
             SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) == FALSE ) {
        return std::unexpected("无法读取 Windows 系统 UI 字体");
    }
    // NONCLIENTMETRICS 的 lfMessageFont 代表当前用户 UI
    // 偏好，而非固定系统默认。

    ComPtr<IDWriteFactory> factory;
    const auto             factoryResult = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    if ( FAILED(factoryResult) ) {
        // shared factory 只在初始化阶段使用，不进入 AppWindow 的持久状态。
        return std::unexpected("无法创建 DirectWrite factory");
    }

    // 消息字体是当前用户的 UI 偏好；异常注册表值收敛到 DirectWrite 合法字重。
    const auto weight = static_cast<DWRITE_FONT_WEIGHT>(
        std::clamp(metrics.lfMessageFont.lfWeight,
                   static_cast<LONG>(DWRITE_FONT_WEIGHT_THIN),
                   static_cast<LONG>(DWRITE_FONT_WEIGHT_ULTRA_BLACK)));
    const auto style = metrics.lfMessageFont.lfItalic != FALSE
                           ? DWRITE_FONT_STYLE_ITALIC
                           : DWRITE_FONT_STYLE_NORMAL;
    // 字体 family、weight 与 italic 共同表达用户偏好，不能只读取族名。
    auto primary = resolveFamily(
        *factory.Get(), metrics.lfMessageFont.lfFaceName, weight, style);
    if ( !primary ) {
        // 系统指标中的第三方字体可能不可由 ImGui 读取，Segoe UI
        // 是明确的本地降级。
        primary = resolveFamily(*factory.Get(),
                                L"Segoe UI",
                                DWRITE_FONT_WEIGHT_NORMAL,
                                DWRITE_FONT_STYLE_NORMAL);
    }
    // 只有主字体成功后才开始收集可选回退，保证返回列表首项契约。
    if ( !primary ) return std::unexpected(std::move(primary.error()));
    // 两级主字体均失败才整体降级；具体错误交给 AppWindow 决定内建字体策略。

    std::vector<SystemFontFace> faces;
    // 主字体加三个 CJK 族和 Emoji，预留小容量避免追加过程反复分配。
    faces.reserve(4);
    appendUnique(faces, std::move(*primary));
    // 主偏好字体固定在首项，后续族只补充缺字，不能覆盖常规 UI 字形。

    // DirectWrite 的平台级 fallback 不参与 ImGui 的 stb_truetype 渲染，因此把
    // Windows 常见系统 CJK 与 Emoji UI 字体作为缺字回退源合并。
    constexpr std::array FALLBACK_FAMILIES{ L"Microsoft YaHei UI",
                                            L"Yu Gothic UI",
                                            L"Malgun Gothic",
                                            L"Segoe UI Emoji" };
    // 顺序决定缺字优先级：中文、日文、韩文，最后才尝试彩色/符号族。
    for ( const auto* family : FALLBACK_FAMILIES ) {
        // 单个回退族缺失不否定已解析的主字体，适配不同 Windows 语言包组合。
        auto fallback = resolveFamily(*factory.Get(),
                                      family,
                                      DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL);
        if ( fallback ) appendUnique(faces, std::move(*fallback));
    }
    return faces;
}

}  // namespace AudioRoads::UI
