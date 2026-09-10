# AudioRoads 架构说明

## 目标

AudioRoads 将任何可采集端点抽象为 source，将任何可播放端点抽象为 sink。用户
可以建立任意多条路由，每条路由独立控制增益、静音、通道矩阵和采样率转换；同一
source 的采集流应被多个路由共享，同一 sink 的所有输入应在一个实时回调中汇总。

## 分层

依赖方向固定为：

```text
Main -> UI -----> Core
  └----> Audio -> Core
```

- Core 只保存值类型、图约束和不依赖平台的 DSP，不包含窗口或操作系统头文件。
- Audio 拥有平台句柄和实时线程，通过 `IAudioBackend` 把设备发现结果转换为
  `AudioDevice`。未来的流接口也必须停留在这一层。
- UI 每帧只读取和编辑内存模型。设备刷新、打开流和关闭流是帧外低频动作。
- Main 负责对象生命周期和动作调度，不包含 DSP 或平台 API 细节。

## 实时路由执行设计

后续设备流实现按以下顺序扩展，不改变现有 UI 和路由图接口：

1. `IAudioBackend` 增加异步设备变更通知和按稳定 ID 打开 capture/render stream。
2. 每个 source 只有一个原生采集流，写入预分配的单生产者环形缓冲区。
3. 每个 sink 只有一个渲染回调，从所有关联 source 的环形缓冲读取数据。
4. 不同设备时钟域之间使用占用量反馈驱动的重采样，禁止用 sleep 或阻塞锁对齐。
5. 通道矩阵、路由增益和静音以不可变快照原子切换；音频回调不复制共享所有权。
6. `mixAudio` 负责最终汇总和限幅，回调中的所有工作区在启动流前完成分配。

Windows 使用事件驱动 WASAPI shared/exclusive stream，Linux 使用 `pw_stream` 和
PipeWire 实时 process 回调，macOS 使用 CoreAudio IOProc。虚拟设备或系统输出
捕获是平台能力：Windows loopback、PipeWire monitor node、macOS Core Audio tap
应转换为普通 source，而不是让 Core 增加平台特例。

## 当前交付边界

当前阶段已经验证路由图不变量、块混音算法和无 PipeWire 开发包时的客户端构建。
原生枚举实现已落到三平台编译分支，但 Windows/macOS 尚未在对应主机编译；Linux
本机缺少 `libpipewire-0.3-dev`，因此 PipeWire 分支尚未完成本机编译和运行验收。
这些边界必须在后续验收报告中继续明确区分。
