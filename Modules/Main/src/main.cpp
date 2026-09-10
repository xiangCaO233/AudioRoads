#include "Application.h"

/// @brief AudioRoads 桌面客户端进程入口。
/// @return Application::run 的退出码：零为正常关闭，非零为启动失败。
int main()
{
    // 进程状态留在栈上，run 返回后按成员逆序确定性释放各平台 RAII 资源。
    // 可恢复设备错误留在 UI 内，只有窗口初始化失败才通过非零退出码上报。
    AudioRoads::Application application;
    // 入口原样转发应用退出码，不捕获异常或吞掉启动故障。
    return application.run();
}
