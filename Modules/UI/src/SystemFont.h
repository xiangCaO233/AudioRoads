#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace AudioRoads::UI
{

/// @brief ImGui 可直接加载的单个系统字体文件及集合索引。
///
/// 这是不暴露 Fontconfig、DirectWrite 或 CoreText 句柄的值对象，便于 UI 层
/// 在平台查询结束后独立建立字体图集。
struct SystemFontFace {
    /// @brief 查询时已存在的本地绝对字体路径。
    std::string path;

    /// @brief TTF/OTF 集合内的字形面索引，普通字体文件为零。
    std::uint32_t faceIndex{};
};

/// @brief 解析平台首选 UI 字体，并附加系统提供的中日韩回退字体。
///
/// 返回顺序决定 ImGui 合并优先级：第一个字体负责常规 UI 字形，其余字体只在
/// 前面的字体缺少字符时补充。函数只允许在应用初始化阶段调用，可以访问字体
/// 配置和文件系统；失败通过 expected 返回，调用方负责降级到 ImGui 内建字体。
[[nodiscard]] std::expected<std::vector<SystemFontFace>, std::string>
resolveSystemFontFaces();

}  // namespace AudioRoads::UI
