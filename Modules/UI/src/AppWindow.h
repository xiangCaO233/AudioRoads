#pragma once

#include <expected>
#include <string>

struct GLFWwindow;

namespace AudioRoads::UI
{

/// @brief 管理 GLFW、OpenGL context 与 ImGui 后端的桌面窗口。
///
/// 资源只能由 initialize 建立，并按 renderer、platform、ImGui、window、GLFW
/// 的逆序释放。对象不可复制或移动，避免 GLFW 全局状态及原生窗口被重复释放。
/// 本类不持有主视图或路由图，只定义渲染生命周期边界。
class AppWindow final
{
public:
    AppWindow() = default;

    /// @brief 按逆序释放 ImGui 和 GLFW 资源。
    ~AppWindow();

    AppWindow(const AppWindow&)            = delete;
    AppWindow& operator=(const AppWindow&) = delete;
    AppWindow(AppWindow&&)                 = delete;
    AppWindow& operator=(AppWindow&&)      = delete;

    /// @brief 创建窗口并初始化 ImGui context、系统字体和官方渲染后端。
    /// @return 任一步失败均返回可读错误；已经建立的资源仍可由析构安全回收。
    /// @pre 每个 AppWindow 实例只调用一次，所有窗口与渲染调用都位于主线程。
    [[nodiscard]] std::expected<void, std::string> initialize();

    /// @brief 返回用户是否已经请求关闭主窗口。
    [[nodiscard]] bool shouldClose() const noexcept;

    /// @brief 轮询系统事件并开始新的 UI 帧。
    /// @warning 每个显示帧调用；禁止加入阻塞等待或文件系统访问。
    void beginFrame();

    /// @brief 提交 ImGui draw data 并交换 OpenGL 缓冲区。
    /// @warning 每个显示帧调用；禁止加入音频设备刷新等低频工作。
    void endFrame();

private:
    /// @brief 由 GLFW 创建并销毁的非拥有类型句柄。
    GLFWwindow* m_window{};

    /// @brief 标记 ImGui context 是否已创建，允许初始化中途失败后精确清理。
    bool m_imguiInitialized{};

    /// @brief 标记 GLFW 全局状态是否由本实例初始化。
    bool m_glfwInitialized{};
};

}  // namespace AudioRoads::UI
