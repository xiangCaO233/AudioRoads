#include "Application.h"

#include <string>

namespace AudioRoads
{

int Application::run()
{
    auto initialized = m_window.initialize();
    if ( !initialized ) return 1;

    refreshDevices();
    while ( !m_window.shouldClose() ) {
        m_window.beginFrame();
        const auto actions = m_mainView.draw(m_audioService.routingGraph(),
                                             m_audioService.backendName());
        m_window.endFrame();

        // 平台枚举可能执行同步 round-trip，只在帧提交后响应显式动作。
        if ( actions.refreshDevices ) refreshDevices();
    }
    return 0;
}

void Application::refreshDevices()
{
    auto refreshed = m_audioService.refreshDevices();
    if ( !refreshed ) {
        m_mainView.setError(refreshed.error().operation + ": " +
                            refreshed.error().message);
    } else {
        m_mainView.setError({});
    }
}

}  // namespace AudioRoads
