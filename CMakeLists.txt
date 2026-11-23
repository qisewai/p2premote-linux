cmake_minimum_required(VERSION 3.23)
project(p2premote VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "p2premote requires a 64-bit Windows build with MSVC")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

set(P2PREMOTE_MINIRTC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/out/minirtc" CACHE PATH "Directory containing minirtc.h and p2premote_minirtc.dll")
foreach(file minirtc.h p2premote_minirtc.dll)
    if(NOT EXISTS "${P2PREMOTE_MINIRTC_DIR}/${file}")
        message(FATAL_ERROR "Build MiniRTC first with tools/minirtc_probe/Build.ps1. Missing ${file} in ${P2PREMOTE_MINIRTC_DIR}")
    endif()
endforeach()

enable_testing()

add_library(input_protocol STATIC proto/input_protocol.cpp)
target_include_directories(input_protocol PUBLIC proto/include)
target_compile_features(input_protocol PUBLIC cxx_std_20)
target_compile_options(input_protocol PRIVATE /W4 /permissive- /utf-8)

add_library(plugin_protocol STATIC proto/plugin_protocol.cpp)
target_include_directories(plugin_protocol PUBLIC proto/include)
target_compile_features(plugin_protocol PUBLIC cxx_std_20)
target_compile_options(plugin_protocol PRIVATE /W4 /permissive- /utf-8)

include(cmake/lua-target.cmake)
add_library(plugin_host STATIC core/src/plugin_host.cpp core/src/clipboard_bridge.cpp)
target_compile_features(plugin_host PRIVATE cxx_std_20)
target_compile_definitions(plugin_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(plugin_host PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(plugin_host PRIVATE plugin_protocol p2premote_lua bcrypt user32 windowsapp)

add_library(core STATIC
    core/src/d3d11_preview.cpp
    core/src/d3d11_video.cpp
    core/src/wgc_capturer.cpp
    core/src/dxgi_capturer.cpp
    core/src/stream_session.cpp
    core/src/minirtc_call.cpp
    core/src/signal_socket.cpp
    core/src/desktop_input.cpp
    core/src/cursor_shape.cpp
    core/src/latency_trace.cpp
    core/src/connection_options.cpp
    core/src/service_identity.cpp
)
target_include_directories(core PUBLIC core/include PRIVATE "${P2PREMOTE_MINIRTC_DIR}" tools/minirtc_probe)
target_compile_features(core PUBLIC cxx_std_20)
target_compile_definitions(core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(core PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(core PUBLIC input_protocol PRIVATE plugin_host d3d11 dxgi d3dcompiler dxguid crypt32 windowsapp)

add_library(stream_window STATIC viewer/stream_window.cpp)
target_link_libraries(stream_window PUBLIC core PRIVATE comdlg32 shell32 uxtheme)
target_compile_options(stream_window PRIVATE /W4 /permissive- /utf-8)

add_executable(p2premote WIN32 viewer/account_main.cpp viewer/account_api.cpp)
include(cmake/curl-target.cmake)
target_link_libraries(p2premote PRIVATE stream_window advapi32 windowsapp CURL::libcurl)
target_compile_options(p2premote PRIVATE /W4 /permissive- /utf-8)
target_link_options(p2premote PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")
set_property(TARGET p2premote APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")

add_custom_target(p2premote_minirtc_runtime
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${P2PREMOTE_MINIRTC_DIR}/p2premote_minirtc.dll" "$<TARGET_FILE_DIR:p2premote>"
    VERBATIM)
add_dependencies(p2premote p2premote_minirtc_runtime)

set(clipboard_plugin "${CMAKE_CURRENT_SOURCE_DIR}/plugins/clipboard-sync")
add_custom_target(p2premote_plugins
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:p2premote>/plugins/clipboard-sync"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${clipboard_plugin}/plugin.json" "${clipboard_plugin}/main.lua"
        "$<TARGET_FILE_DIR:p2premote>/plugins/clipboard-sync"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${P2PREMOTE_LUA_NOTICE}" "$<TARGET_FILE_DIR:p2premote>/lua-notice.html"
    VERBATIM)
add_dependencies(p2premote p2premote_plugins)

add_executable(input_protocol_test proto/input_protocol_test.cpp)
target_link_libraries(input_protocol_test PRIVATE input_protocol)
target_compile_options(input_protocol_test PRIVATE /W4 /permissive- /utf-8)
add_test(NAME input_protocol_test COMMAND input_protocol_test)

add_executable(plugin_protocol_test proto/plugin_protocol_test.cpp)
target_link_libraries(plugin_protocol_test PRIVATE plugin_protocol)
target_compile_options(plugin_protocol_test PRIVATE /W4 /permissive- /utf-8)
add_test(NAME plugin_protocol_test COMMAND plugin_protocol_test)

add_executable(plugin_host_test core/src/plugin_host_test.cpp)
target_link_libraries(plugin_host_test PRIVATE plugin_host plugin_protocol user32)
target_compile_definitions(plugin_host_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(plugin_host_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
add_test(NAME plugin_host_test COMMAND plugin_host_test)
add_test(NAME clipboard_plugin_test COMMAND plugin_host_test "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
set_tests_properties(plugin_host_test clipboard_plugin_test PROPERTIES TIMEOUT 20)
set_tests_properties(clipboard_plugin_test PROPERTIES SKIP_RETURN_CODE 77)
