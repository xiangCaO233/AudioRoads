#include "SystemFont.h"

#include <cassert>
#include <filesystem>
#include <system_error>

/// @brief 验证当前桌面环境至少能解析一个真实存在的系统字体文件。
///
/// 测试链接产品使用的同一平台实现，但不创建 GLFW 或 ImGui context，因此只
/// 证明系统偏好可解析为本地文件；字号、合并效果和视觉质量仍需运行客户端验收。
int main()
{
    // 不固定字体族或回退数量：结果必须遵循运行测试的用户桌面配置。
    const auto fonts = AudioRoads::UI::resolveSystemFontFaces();
    assert(fonts.has_value());
    // expected 失败代表平台解析不可用，不应被误当作“系统没有回退字体”。
    // 第一项是 AppWindow 将采用的主 UI 字体，成功结果不得只有空回退列表。
    assert(!fonts->empty());

    for ( const auto& face : *fonts ) {
        // 主字体和每个回退都走同一文件契约，不能只验证列表首项。
        assert(!face.path.empty());
        // 使用 error_code 保持项目无异常约束，并分别验证路径类型和查询错误。
        std::error_code error;
        assert(std::filesystem::is_regular_file(face.path, error));
        // 独立断言 error_code，区分“不存在/非普通文件”与文件系统查询本身失败。
        assert(!error);
    }
    return 0;
}
