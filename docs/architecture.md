# AudioRoads 架构说明

## 目标

AudioRoads 将物理录音设备和单个应用的播放输出抽象为 source，将物理播放设备和
系统可见虚拟麦克风抽象为 target。用户可以建立任意多条有向路由，每条路由独立
控制增益、静音、通道矩阵和采样率转换；同一 source 的采集流被多条路由共享，
同一 target 的所有输入在一个实时回调中汇总。

## 分层

依赖方向固定为：

```text
Main -> UI -----> Core
  └----> Audio -> Core
```

- Core 只保存值类型、图约束和不依赖平台的 DSP，不包含窗口或操作系统头文件。
- Audio 拥有平台句柄和实时线程，通过 `IAudioBackend` 把发现结果转换为
  `AudioSource` 与 `AudioTarget`。流接口和平台组件 IPC 也必须停留在这一层。
- UI 每帧只读取和编辑内存模型。设备刷新、打开流和关闭流是帧外低频动作。
- Main 负责对象生命周期和动作调度，不包含 DSP 或平台 API 细节。

## 实时路由执行设计

设备流实现按以下顺序扩展，不改变现有 UI 和路由图接口：

1. `IAudioBackend` 增加异步端点变更通知和按稳定 ID 打开 capture/render stream。
2. 每个 source 只有一个原生采集流，写入预分配的 `SpscAudioRingBuffer`。
3. 每个 target 只有一个消费回调，从所有关联 source 的环形缓冲读取数据。
4. 不同设备时钟域之间使用占用量反馈驱动的重采样，禁止用 sleep 或阻塞锁对齐。
5. 通道矩阵、路由增益和静音以不可变快照原子切换；音频回调不复制共享所有权。
6. `mixAudio` 负责最终汇总和限幅，回调中的所有工作区在启动流前完成分配。

Windows 使用事件驱动 WASAPI 与 application loopback，Linux 使用 `pw_stream` 和
PipeWire 实时 process 回调，macOS 使用 CoreAudio IOProc 与 process tap。应用
输出捕获均转换为普通 source；虚拟麦克风则是持续消费混音块的 target，只有平台
组件在操作系统侧把它呈现为录音设备，Core 不增加驱动特例。

## 节点画布语义

- 来源方块只有输出端口，可同时连接多个目标。
- 目标方块只有输入端口，多条入边表示在该目标回调内混音。
- 连接线对应一条 `AudioRoute`，增益和静音属于连接而不是端点。
- 端点离线时路由配置继续保留；稳定 ID 再次出现后执行层重新接通。
- 画布每帧只操作 `RoutingGraph`，不能直接创建驱动设备或打开平台流。

## 当前交付边界

当前阶段已经验证路由图不变量、块混音算法、无锁 SPSC 缓冲和 Linux PipeWire
产品构建。三平台应用输出发现已落到各自编译分支，但 Windows/macOS 尚未在对应
主机编译，原生捕获/播放流与虚拟麦克风组件也尚未接通。后续验收必须继续区分
“发现端点”“建立数据流”“系统识别虚拟设备”三个证据层级。
