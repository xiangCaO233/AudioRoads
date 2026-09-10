#include "AppWindow.h"

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
    if ( m_imguiInitialized ) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    if ( m_window != nullptr ) glfwDestroyWindow(m_window);
    if ( m_glfwInitialized ) glfwTerminate();
}

std::expected<void, std::string> AppWindow::initialize()
{
    glfwSetErrorCallback([](int, const char*) {
        // 初始化错误由各 GLFW 返回值汇总，回调不得直接写控制台。
    });
    if ( glfwInit() != GLFW_TRUE ) {
        return std::unexpected("GLFW initialization failed");
    }
    m_glfwInitialized = true;

#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    constexpr auto* GLSL_VERSION = "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    constexpr auto* GLSL_VERSION = "#version 130";
#endif

    m_window = glfwCreateWindow(1280, 760, "AudioRoads", nullptr, nullptr);
    if ( m_window == nullptr ) {
        return std::unexpected("GLFW window creation failed");
    }
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    if ( !ImGui_ImplGlfw_InitForOpenGL(m_window, true) ) {
        ImGui::DestroyContext();
        return std::unexpected("ImGui GLFW backend initialization failed");
    }
    if ( !ImGui_ImplOpenGL3_Init(GLSL_VERSION) ) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return std::unexpected("ImGui OpenGL backend initialization failed");
    }
    m_imguiInitialized = true;
    return {};
}

bool AppWindow::shouldClose() const noexcept
{
    return m_window == nullptr || glfwWindowShouldClose(m_window) != 0;
}

void AppWindow::beginFrame()
{
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void AppWindow::endFrame()
{
    ImGui::Render();
    int width{};
    int height{};
    glfwGetFramebufferSize(m_window, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(0.035F, 0.043F, 0.055F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(m_window);
}

}  // namespace AudioRoads::UI
