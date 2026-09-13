# AudioRoads 平台端点组件

## 统一边界

路由引擎只处理交错 `float32` PCM 数据。物理播放设备和虚拟麦克风都从引擎角度
消费混音结果；区别只存在于 Audio 层：前者写入系统播放流，后者把同一数据交给
平台组件，使其他软件能把它作为录音来源打开。虚拟麦克风没有上游数据时必须持续
提供静音，不得让操作系统读取线程等待 UI 或路由线程。

主程序与独立组件之间采用固定格式头、共享内存 SPSC 环形缓冲和轻量控制通道。
控制通道负责版本协商、格式、启动与停止；共享缓冲只传 PCM。双方不得在实时回调
中分配、加锁、写日志或复制所有权。组件断开时主程序保留画布配置，并把对应目标
标为离线。

## Windows

- 单应用来源使用 Microsoft Application Loopback 的进程树捕获模式；同一稳定
  应用标识下的多个活动 PID 分别建立捕获流，再在 Core 层汇总。
- 系统可见麦克风使用基于 SysVAD/WaveRT 的独立 AVStream 驱动。主程序不能仅靠
  普通用户态进程注册一个所有应用均可打开的 WASAPI capture endpoint。
- 开发包使用 Windows 官方测试证书和测试签名模式。发布包只接受正规签名及对应
  的驱动分发流程；项目不实现证书校验绕过、盗用证书或所谓“黑签”方案。
- 驱动只负责端点、格式协商与持续消费共享缓冲，路由图、混音和 UI 全部留在普通
  用户态客户端，缩小内核代码面。

参考：Microsoft 的
[Application Loopback 示例](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
与 [SysVAD 示例](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad)。

## Linux / PipeWire

- 应用来源来自 `Stream/Output/Audio` 节点，并按应用 binary/name 聚合；打开路由时
  解析当前匹配节点，不能依赖短生命周期 registry ID。
- 物理来源和聚合应用来源已经通过定向 capture `pw_stream` 接入；每个原生来源只
  打开一次，并向每条出边的独立 SPSC 缓冲扇出统一的双声道 float32 PCM。
- 虚拟麦克风由 AudioRoads 加载 PipeWire loopback 模块：公开的 `Audio/Source`
  节点供其他应用当作录音设备，关闭自动连接的内部 `Stream/Input/Audio` 只接收
  AudioRoads 的目标混音 playback。节点对在后端寿命内常驻，增删路由不会让正在
  录音的应用丢失设备；不需要内核驱动，后续可由用户服务托管获得跨 UI 重启的
  持久性。
- 物理目标是普通 `Audio/Sink`，当前已由唯一 playback 回调持续拉取全部入边、
  应用连接增益/静音并播放混音结果。PipeWire 负责 48 kHz 双声道引擎格式与设备
  原生格式之间的基础转换；长期跨硬件时钟漂移校正仍需后续占用量反馈重采样。

参考：PipeWire 的
[loopback module](https://docs.pipewire.org/page_module_loopback.html) 端点属性模型。

## macOS

- macOS 14.2 及以上使用 Core Audio process tap 捕获指定进程或进程组的输出；应用
  需要声明系统音频捕获用途并正确处理用户权限拒绝。
- 系统可见麦克风由独立 Audio Server Plug-in 发布。插件运行在音频服务进程中，
  通过共享内存读取主程序混音结果，不能直接链接或回调 UI 对象。
- 安装、签名、公证、升级和卸载必须作为独立交付生命周期处理；插件崩溃或版本不
  匹配时，客户端必须保持可启动并报告目标离线。

参考：Apple 的
[Core Audio taps](https://developer.apple.com/documentation/coreaudio/capturing-system-audio-with-core-audio-taps)
与
[Audio Server Plug-in](https://developer.apple.com/documentation/coreaudio/creating-an-audio-server-driver-plug-in)。
