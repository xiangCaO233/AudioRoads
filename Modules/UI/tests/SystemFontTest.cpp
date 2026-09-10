#include "SystemFont.h"

#include <cassert>
#include <filesystem>
#include <system_error>

/// @brief 验证当前桌面环境至少能解析一个真实存在的系统字体文件。
int main()
{
    const auto fonts = AudioRoads::UI::resolveSystemFontFaces();
    assert(fonts.has_value());
    assert(!fonts->empty());

    for ( const auto& face : *fonts ) {
        assert(!face.path.empty());
        std::error_code error;
        assert(std::filesystem::is_regular_file(face.path, error));
        assert(!error);
    }
    return 0;
}
