cmake_minimum_required(VERSION 3.23)
option(P2PREMOTE_BUILD_QT "Build the Qt 6 Widgets frontend" OFF)
if(P2PREMOTE_BUILD_QT)
    list(APPEND VCPKG_MANIFEST_FEATURES qt-ui)
endif()
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
add_library(plugin_host STATIC core/src/plugin_host.cpp core/src/clipboard_bridge.cpp core/src/file_clipboard.cpp core/src/plugin_settings.cpp core/src/pty_session.cpp)
target_include_directories(plugin_host PUBLIC core/include)
target_compile_features(plugin_host PRIVATE cxx_std_20)
target_compile_definitions(plugin_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(plugin_host PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(plugin_host PUBLIC input_protocol PRIVATE plugin_protocol p2premote_lua bcrypt user32 imm32 shell32 ole32 windowsapp)

add_library(core STATIC
    core/src/audio_engine.cpp
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
    core/src/viewer_input.cpp
)
target_include_directories(core PUBLIC core/include PRIVATE "${P2PREMOTE_MINIRTC_DIR}" tools/minirtc_probe)
target_compile_features(core PUBLIC cxx_std_20)
target_compile_definitions(core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(core PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(core PUBLIC input_protocol PRIVATE plugin_host d3d11 dxgi d3dcompiler dxguid crypt32 ole32 avrt mmdevapi windowsapp)

add_library(stream_window STATIC viewer/stream_window.cpp viewer/connection_overlay.cpp)
target_include_directories(stream_window PRIVATE core/src)
target_link_libraries(stream_window PUBLIC core PRIVATE comdlg32 shell32 uxtheme)
target_compile_options(stream_window PRIVATE /W4 /permissive- /utf-8)

include(cmake/curl-target.cmake)
add_library(account_service STATIC core/src/account_api.cpp)
target_include_directories(account_service PRIVATE core/include)
target_compile_features(account_service PRIVATE cxx_std_20)
target_compile_definitions(account_service PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(account_service PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(account_service PRIVATE advapi32 windowsapp CURL::libcurl)

add_library(p2premote_core SHARED core/src/core_account_api.cpp core/src/core_stream_api.cpp)
target_include_directories(p2premote_core INTERFACE core/include)
target_compile_definitions(p2premote_core PRIVATE P2PREMOTE_CORE_EXPORTS)
target_compile_options(p2premote_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(p2premote_core PRIVATE core account_service comctl32)

add_executable(p2premote WIN32 viewer/account_main.cpp)
target_include_directories(p2premote PRIVATE core/src)
target_link_libraries(p2premote PRIVATE stream_window account_service)
target_compile_options(p2premote PRIVATE /W4 /permissive- /utf-8)
target_link_options(p2premote PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")
set_property(TARGET p2premote APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")

add_executable(p2premote_service WIN32 host/service_main.cpp)
target_compile_features(p2premote_service PRIVATE cxx_std_20)
target_compile_definitions(p2premote_service PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(p2premote_service PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(p2premote_service PRIVATE p2premote_core advapi32 crypt32 delayimp shell32 userenv wtsapi32 windowsapp)
target_link_options(p2premote_service PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest"
    "/DELAYLOAD:p2premote_core.dll")
set_property(TARGET p2premote_service APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")

add_custom_target(p2premote_minirtc_runtime
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${P2PREMOTE_MINIRTC_DIR}/p2premote_minirtc.dll" "$<TARGET_FILE_DIR:p2premote>"
    VERBATIM)
add_dependencies(p2premote p2premote_minirtc_runtime)

set(plugin_assets)
set(P2PREMOTE_BUILTIN_PLUGINS clipboard-sync file-clipboard shortcut-macros keyboard-sync adaptive-quality file-transfer remote-terminal connection-status session-stopwatch)
foreach(plugin IN LISTS P2PREMOTE_BUILTIN_PLUGINS)
    set(plugin_dir "${CMAKE_CURRENT_SOURCE_DIR}/plugins/${plugin}")
    set(plugin_output "$<TARGET_FILE_DIR:p2premote>/plugins/${plugin}")
    list(APPEND plugin_assets
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${plugin_output}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${plugin_dir}/plugin.json" "${plugin_dir}/main.lua" "${plugin_output}")
endforeach()
add_custom_target(p2premote_plugins
    ${plugin_assets}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${P2PREMOTE_LUA_NOTICE}" "$<TARGET_FILE_DIR:p2premote>/lua-notice.html"
    VERBATIM)
add_dependencies(p2premote p2premote_plugins)
add_dependencies(p2premote_core p2premote_minirtc_runtime p2premote_plugins)

if(P2PREMOTE_BUILD_QT)
    include(cmake/qt-frontend.cmake)
endif()

add_executable(core_api_test core/src/core_api_test.c)
target_link_libraries(core_api_test PRIVATE p2premote_core)
target_compile_options(core_api_test PRIVATE /W4 /utf-8)
add_test(NAME core_api_test COMMAND core_api_test)
set_tests_properties(core_api_test PROPERTIES TIMEOUT 12)

add_executable(idle_scroll_test tools/minirtc_probe/idle_scroll_test.cpp)
target_include_directories(idle_scroll_test PRIVATE core/src tools/minirtc_probe "${P2PREMOTE_MINIRTC_DIR}")
target_link_libraries(idle_scroll_test PRIVATE core windowsapp)
target_compile_options(idle_scroll_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
add_dependencies(idle_scroll_test p2premote_minirtc_runtime)
add_test(NAME idle_scroll_test COMMAND idle_scroll_test)
set_tests_properties(idle_scroll_test PROPERTIES LABELS "gpu;loopback" TIMEOUT 60 SKIP_RETURN_CODE 77)

add_executable(dxgi_cursor_check_test core/src/dxgi_cursor_check_test.cpp)
target_compile_features(dxgi_cursor_check_test PRIVATE cxx_std_20)
add_test(NAME dxgi_cursor_check_test COMMAND dxgi_cursor_check_test)

add_executable(input_protocol_test proto/input_protocol_test.cpp)
target_link_libraries(input_protocol_test PRIVATE input_protocol)
target_compile_options(input_protocol_test PRIVATE /W4 /permissive- /utf-8)
add_test(NAME input_protocol_test COMMAND input_protocol_test)

add_executable(audio_engine_test core/src/audio_engine_test.cpp core/src/audio_engine.cpp)
target_include_directories(audio_engine_test PRIVATE core/src)
target_compile_features(audio_engine_test PRIVATE cxx_std_20)
target_compile_definitions(audio_engine_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(audio_engine_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(audio_engine_test PRIVATE ole32 avrt mmdevapi windowsapp)
add_test(NAME audio_engine_test COMMAND audio_engine_test)

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
add_test(NAME file_clipboard_plugin_test COMMAND plugin_host_test --file-clipboard "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
add_test(NAME office_plugin_test COMMAND plugin_host_test --office "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
add_test(NAME file_transfer_plugin_test COMMAND plugin_host_test --file-transfer "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
add_test(NAME remote_terminal_plugin_test COMMAND plugin_host_test --remote-terminal "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
add_test(NAME connection_status_plugin_test COMMAND plugin_host_test --connection-status "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
set_tests_properties(connection_status_plugin_test PROPERTIES TIMEOUT 10)
add_executable(connection_overlay_test viewer/connection_overlay_test.cpp)
target_link_libraries(connection_overlay_test PRIVATE stream_window comctl32)
target_compile_options(connection_overlay_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_options(connection_overlay_test PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")
add_test(NAME connection_overlay_test COMMAND connection_overlay_test)
set_tests_properties(connection_overlay_test PROPERTIES TIMEOUT 10)
set_tests_properties(plugin_host_test clipboard_plugin_test file_clipboard_plugin_test office_plugin_test file_transfer_plugin_test remote_terminal_plugin_test PROPERTIES TIMEOUT 20)
set_tests_properties(clipboard_plugin_test file_clipboard_plugin_test PROPERTIES SKIP_RETURN_CODE 77)
