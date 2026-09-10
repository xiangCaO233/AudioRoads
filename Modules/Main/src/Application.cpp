#include "Application.h"

#include <string>

namespace AudioRoads
{

int Application::run()
{
    // AppWindow 是 GLFW、ImGui 和 OpenGL 的唯一生命周期所有者；Main 只消费
    // expected 结果，不复制部分初始化的清理规则。
    auto initialized = m_window.initialize();
    // 部分初始化资源由 AppWindow 析构清理，窗口不可用时不能进入事件循环。
    if ( !initialized ) return 1;

    // 首帧前取得快照；失败只成为可见错误，UI 仍可启动并允许用户再次刷新。
    refreshEndpoints();
    while ( !m_window.shouldClose() ) {
        // 窗口关闭是循环唯一终止条件，设备故障不会强制结束可恢复的客户端会话。
        // 每帧严格保持 begin/draw/end 配对，避免动作处理破坏 ImGui 状态栈。
        m_window.beginFrame();
        const auto actions = m_mainView.draw(m_audioService.routingGraph(),
                                             m_audioService.backendName());
        // draw 返回纯值动作且不持有 graph 引用，结束帧后处理不会破坏控件栈。
        m_window.endFrame();

        // 平台枚举可能执行同步 round-trip，只在帧提交后响应显式动作。
        if ( actions.refreshEndpoints ) refreshEndpoints();
        // 没有动作时循环只处理渲染与输入，不进行隐式周期性平台枚举。
    }
    // 正常关闭仅结束循环；栈上成员随后按视图、窗口、音频服务的逆序销毁。
    return 0;
}

void Application::refreshEndpoints()
{
    // 设备服务负责“完整成功才替换”语义，此处只把结果映射为用户可见状态。
    auto refreshed = m_audioService.refreshEndpoints();
    if ( !refreshed ) {
        // 平台错误是值对象，在本作用域读取后立即复制进视图持久字符串。
        // 保留“操作: 原因”两层上下文，且不让可恢复设备错误改变进程退出码。
        m_mainView.setError(refreshed.error().operation + ": " +
                            refreshed.error().message);
    } else {
        // 成功快照替换意味着旧故障已恢复，不能继续展示过期错误。
        m_mainView.setError({});
    }
}

}  // namespace AudioRoads
