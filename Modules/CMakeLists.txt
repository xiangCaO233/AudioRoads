# Core 位于依赖图底层，只定义设备 DTO、路由图和无平台混音算法。
add_subdirectory("Core")
# Audio 依赖 Core，把三平台原生对象转换为值快照，不向上泄漏平台 API。
add_subdirectory("Audio")
# UI 只依赖 Core 编辑内存模型，不得为调用后端而反向依赖 Audio。
add_subdirectory("UI")
# Main 最后汇合 Audio 与 UI 并管理进程生命周期；不能靠调换顺序掩盖反向依赖。
# 新模块应先明确层级再加入，这里只负责一级组装，不声明具体 target 细节。
add_subdirectory("Main")
