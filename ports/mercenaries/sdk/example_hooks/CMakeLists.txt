cmake_minimum_required(VERSION 3.20)
project(mercenaries_hooks_example_plugin C)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build this plugin for Windows x64.")
endif()
add_library(hooks_example SHARED hooks_example.c)
set_target_properties(hooks_example PROPERTIES PREFIX "" C_STANDARD 11
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(hooks_example PRIVATE /W4 /WX /utf-8)
endif()
add_custom_command(TARGET hooks_example POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/mod.ini" "$<TARGET_FILE_DIR:hooks_example>/mod.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/hooks_example.ini" "$<TARGET_FILE_DIR:hooks_example>/hooks_example.ini"
    COMMAND ${CMAKE_COMMAND} -E echo "Plugin folder: $<TARGET_FILE_DIR:hooks_example>")

