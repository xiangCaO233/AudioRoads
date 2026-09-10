include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_glfw_include_dir glfw)

if(NOT TARGET glfw)
  add_library(glfw UNKNOWN IMPORTED GLOBAL)
  set_target_properties(glfw PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                        "${_glfw_include_dir}")
  prebuilt_target_configs(_glfw_configs)
  set(_glfw_imported_configs "")
  set(_glfw_default_library "")
  foreach(_config IN LISTS _glfw_configs)
    string(TOUPPER "${_config}" _config_upper)
    prebuilt_find_library(_library glfw "${_config}" glfw glfw3)
    list(APPEND _glfw_imported_configs "${_config_upper}")
    set_target_properties(glfw PROPERTIES "IMPORTED_LOCATION_${_config_upper}"
                                          "${_library}")
    if(_glfw_default_library STREQUAL "")
      set(_glfw_default_library "${_library}")
    endif()
  endforeach()
  set_target_properties(
    glfw
    PROPERTIES IMPORTED_CONFIGURATIONS "${_glfw_imported_configs}"
               IMPORTED_LOCATION "${_glfw_default_library}"
               IMPORTED_LOCATION_NOCONFIG "${_glfw_default_library}")
  target_link_libraries(glfw INTERFACE OpenGL::GL Threads::Threads)
  if(WIN32)
    target_link_libraries(glfw INTERFACE gdi32)
  elseif(APPLE)
    find_library(_glfw_cocoa Cocoa REQUIRED)
    find_library(_glfw_iokit IOKit REQUIRED)
    find_library(_glfw_quartz_core QuartzCore REQUIRED)
    find_library(_glfw_core_foundation CoreFoundation REQUIRED)
    target_link_libraries(
      glfw INTERFACE "${_glfw_cocoa}" "${_glfw_iokit}" "${_glfw_quartz_core}"
                     "${_glfw_core_foundation}")
  else()
    target_link_libraries(glfw INTERFACE ${CMAKE_DL_LIBS} m)
  endif()
endif()

if(NOT TARGET 3rd_glfw)
  add_library(3rd_glfw INTERFACE)
  target_link_libraries(3rd_glfw INTERFACE glfw)
endif()

set(glfw_FOUND TRUE)
