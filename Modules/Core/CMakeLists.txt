add_library(AudioRoadsCore STATIC src/Mixer.cpp src/RoutingGraph.cpp)
target_include_directories(AudioRoadsCore
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(AudioRoadsCore
                        PUBLIC cxx_std_${AUDIOROADS_CXX_STANDARD})
target_link_libraries(AudioRoadsCore PRIVATE AudioRoadsWarnings)

if(BUILD_TESTING)
  add_executable(RoutingGraphTest tests/RoutingGraphTest.cpp)
  target_link_libraries(RoutingGraphTest PRIVATE AudioRoadsCore
                                                 AudioRoadsWarnings)
  add_test(NAME RoutingGraphTest COMMAND RoutingGraphTest)

  add_executable(MixerTest tests/MixerTest.cpp)
  target_link_libraries(MixerTest PRIVATE AudioRoadsCore AudioRoadsWarnings)
  add_test(NAME MixerTest COMMAND MixerTest)
endif()
