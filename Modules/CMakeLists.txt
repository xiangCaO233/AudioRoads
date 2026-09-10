# 模块顺序同时表达依赖方向：Core 不依赖平台和 UI，Main 仅负责组装。
add_subdirectory("Core")
add_subdirectory("Audio")
add_subdirectory("UI")
add_subdirectory("Main")
