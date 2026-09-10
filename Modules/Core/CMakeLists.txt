# Core 只包含无平台状态和实时安全算法，不链接 UI、系统音频库或线程库。
# 源列表显式维护，便于审查哪些代码可能进入音频热路径；禁止递归 glob。
add_library(AudioRoadsCore STATIC src/Mixer.cpp src/RoutingGraph.cpp)
# PUBLIC 头路径只供仓库内模块传播，不表示对外安装 SDK。
target_include_directories(AudioRoadsCore
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# 语言标准从顶层唯一缓存变量传入，避免模块之间出现不同 ABI 基线。
target_compile_features(AudioRoadsCore
                        PUBLIC cxx_std_${AUDIOROADS_CXX_STANDARD})
# 告警策略保持 PRIVATE，既覆盖自维护源码，也不泄漏给未来的外部消费者。
target_link_libraries(AudioRoadsCore PRIVATE AudioRoadsWarnings)

if(BUILD_TESTING)
  # 每个测试单独链接 Core，使图约束与混音契约的失败粒度清晰。
  # 测试不安装，但必须同时创建 target 与 add_test，避免只编译未执行。
  # 测试代码沿用产品告警策略，不以降低编译质量换取通过。
  add_executable(RoutingGraphTest tests/RoutingGraphTest.cpp)
  target_link_libraries(RoutingGraphTest PRIVATE AudioRoadsCore
                                                 AudioRoadsWarnings)
  add_test(NAME RoutingGraphTest COMMAND RoutingGraphTest)

  # MixerTest 只验证确定性数值与缓冲区契约，不引入任何平台音频依赖。
  add_executable(MixerTest tests/MixerTest.cpp)
  target_link_libraries(MixerTest PRIVATE AudioRoadsCore AudioRoadsWarnings)
  add_test(NAME MixerTest COMMAND MixerTest)
endif()
