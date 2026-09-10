#include "AppWindow.h"

#include "SystemFont.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <expected>
#include <string>

namespace AudioRoads::UI
{

AppWindow::~AppWindow()
{
    // 严格按初始化的逆序拆除；状态标记使任何中途失败都可进入同一析构路径。
    if ( m_imguiInitialized ) {
        // renderer 持有 OpenGL/ImGui 状态引用，必须先于 platform backend 关闭。
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        // 两个 backend 都停止访问 context 后才能销毁 ImGui 全局上下文。
        ImGui::DestroyContext();
    }
    // 原生窗口依赖 GLFW 全局状态，窗口清理必须位于 glfwTerminate 之前。
    if ( m_window != nullptr ) glfwDestroyWindow(m_window);
    if ( m_glfwInitialized ) glfwTerminate();
}

std::expected<void, std::string> AppWindow::initialize()
{
    // 初始化只允许执行一次；成员所有权标记在每一阶段成功后立即更新，使后续
    // 失败由析构补齐当前函数未显式处理的窗口和 GLFW 资源。
    glfwSetErrorCallback([](int, const char*) {
        // 初始化错误由各 GLFW 返回值汇总，回调不得直接写控制台。
    });
    if ( glfwInit() != GLFW_TRUE ) {
        // GLFW 未获得所有权，保持标记为 false，析构不会调用不匹配的 terminate。
        return std::unexpected("GLFW initialization failed");
    }
    m_glfwInitialized = true;

#if defined(__APPLE__)
    // macOS 只提供 core profile，GLSL 版本必须与请求的 OpenGL 3.2 配套。
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    constexpr auto* GLSL_VERSION = "#version 150";
#else
    // Windows 与现代 Linux 使用 ImGui OpenGL3 后端支持的兼容基线。
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    constexpr auto* GLSL_VERSION = "#version 130";
#endif

    m_window = glfwCreateWindow(1280, 760, "AudioRoads", nullptr, nullptr);
    // 初始尺寸是逻辑窗口大小，实际 framebuffer 与字体缩放随后从 GLFW 查询。
    if ( m_window == nullptr ) {
        // 已初始化的 GLFW 由对象析构回收；此处不提前清理以保持单一所有权路径。
        return std::unexpected("GLFW window creation failed");
    }
    glfwMakeContextCurrent(m_window);
    // VSync 限制空闲工作区的提交频率，避免关闭窗口前持续满速轮询。
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    // ImGui context 必须在字体、样式或 backend 配置前存在；backend 成功之前
    // m_imguiInitialized 仍为 false，因此失败分支在本函数显式销毁 context。
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 键盘导航是桌面客户端基本可访问性入口，与鼠标输入使用同一帧状态。
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Docking 只改变工作区布局，不允许视图借机保存平台设备对象。
    io.ConfigDpiScaleFonts = true;
    ImGui::StyleColorsDark();
    // 先建立统一基础风格，再只覆盖字号和 DPI，避免字体初始化隐式改色。

    // 首个字体源成为默认 UI 字体，其余系统回退源只补充缺失字形。ImGui 1.92
    // 的动态字体图集会按实际文本请求字形，无需预烘焙完整 CJK 范围。
    bool fontLoaded{};
    if ( auto systemFonts = resolveSystemFontFaces(); systemFonts ) {
        // expected 失败不在此打印日志，统一走内建字体以保证客户端仍可启动。
        for ( const auto& face : *systemFonts ) {
            ImFontConfig config;
            // FontNo 选择 TTC/OTC
            // 内真实字形面；首项建立默认字体，之后只合并缺字。
            config.FontNo    = face.faceIndex;
            config.MergeMode = fontLoaded;
            if ( io.Fonts->AddFontFromFileTTF(
                     face.path.c_str(), 0.0F, &config) != nullptr ) {
                // 单个回退加载失败不撤销之前成功字体，下一候选仍可继续补全字形。
                fontLoaded = true;
            }
        }
    }
    // 平台查询或所有文件加载都失败时保留可启动界面，而不是留下空字体图集。
    if ( !fontLoaded ) io.Fonts->AddFontDefaultVector();

    auto& style = ImGui::GetStyle();
    // 字号保持逻辑像素，显示缩放只作用于 DPI 比例，避免高 DPI 下二次放大。
    style.FontSizeBase = 16.0F;
    float xScale{ 1.0F };
    float yScale{ 1.0F };
    glfwGetWindowContentScale(m_window, &xScale, &yScale);
    // 取两轴较大值避免非等比环境下文字任一方向过小；后续 viewport 可独立缩放。
    style.FontScaleDpi = xScale > yScale ? xScale : yScale;

    if ( !ImGui_ImplGlfw_InitForOpenGL(m_window, true) ) {
        // GLFW backend 尚未取得所有权，仅销毁已建立的 ImGui context。
        ImGui::DestroyContext();
        return std::unexpected("ImGui GLFW backend initialization failed");
    }
    // renderer 初始化依赖已成功的 GLFW backend 和当前 OpenGL context。
    if ( !ImGui_ImplOpenGL3_Init(GLSL_VERSION) ) {
        // renderer 失败时先撤销 platform backend，再销毁它引用的 context。
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return std::unexpected("ImGui OpenGL backend initialization failed");
    }
    m_imguiInitialized = true;
    // 只有两个 backend 均成功后才交由析构统一 shutdown，防止调用未初始化后端。
    return {};
}

bool AppWindow::shouldClose() const noexcept
{
    // 空窗口视为应退出，保护调用方在未来部分初始化路径误入循环时不解引用。
    return m_window == nullptr || glfwWindowShouldClose(m_window) != 0;
}

void AppWindow::beginFrame()
{
    // 顺序遵循官方 GLFW/OpenGL3 集成：平台输入先于 ImGui::NewFrame 汇总。
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    // 两个 backend 都提交本帧状态后，才允许视图开始发出控件命令。
    ImGui::NewFrame();
}

void AppWindow::endFrame()
{
    // Render 先冻结本帧 draw data，之后 OpenGL backend 才能安全读取命令列表。
    ImGui::Render();
    int width{};
    int height{};
    // framebuffer 尺寸包含 DPI 缩放，不能以窗口逻辑尺寸设置 OpenGL viewport。
    glfwGetFramebufferSize(m_window, &width, &height);
    glViewport(0, 0, width, height);
    // viewport 每帧更新以响应窗口尺寸和显示器 DPI 变化，不缓存启动时数值。
    glClearColor(0.035F, 0.043F, 0.055F, 1.0F);
    // 每帧明确清除颜色缓冲，避免空白区域保留交换链中的旧帧内容。
    glClear(GL_COLOR_BUFFER_BIT);
    // 这里只清颜色缓冲；当前 2D ImGui backend 不使用深度或模板附件。
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    // draw data 完全提交后交换前后缓冲，下一帧不会观察半绘制结果。
    glfwSwapBuffers(m_window);
}

}  // namespace AudioRoads::UI
