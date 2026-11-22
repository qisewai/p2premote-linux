cmake_minimum_required(VERSION 3.23)
project(p2premote VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "p2premote requires a 64-bit Windows build with MSVC")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)

option(P2PREMOTE_BUILD_WEBRTC_PROBE "Build the Experiment B WebRTC dependency probe" OFF)
option(P2PREMOTE_BUILD_STREAMING "Build the Experiment B Host and Viewer" OFF)
set(P2PREMOTE_MINIRTC_DIR "" CACHE PATH "Directory containing minirtc.h and p2premote_minirtc.dll")
if(P2PREMOTE_BUILD_WEBRTC_PROBE OR P2PREMOTE_BUILD_STREAMING)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
    include(cmake/webrtc-target.cmake)
endif()

add_library(core STATIC
    core/src/preview_session.cpp
    core/src/preview_stats.cpp
    core/src/d3d11_preview.cpp
    core/src/wgc_capturer.cpp
)
target_include_directories(core PUBLIC core/include)
target_compile_features(core PUBLIC cxx_std_20)
target_compile_definitions(core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(core PRIVATE d3d11 dxgi d3dcompiler windowsapp)

add_executable(p2premote_host WIN32 host/main.cpp)
target_link_libraries(p2premote_host PRIVATE core comdlg32)

add_executable(p2premote_test_pattern WIN32 tools/test_pattern.cpp)
target_compile_features(p2premote_test_pattern PRIVATE cxx_std_20)
target_compile_definitions(p2premote_test_pattern PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(p2premote_test_pattern PRIVATE uxtheme)

foreach(target core p2premote_host p2premote_test_pattern)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
endforeach()

set(gui_targets p2premote_host p2premote_test_pattern)
if(P2PREMOTE_BUILD_STREAMING)
    add_library(input_protocol STATIC proto/input_protocol.cpp)
    target_include_directories(input_protocol PUBLIC proto/include)
    target_compile_features(input_protocol PUBLIC cxx_std_20)
    target_compile_options(input_protocol PRIVATE /W4 /permissive- /utf-8)

    target_sources(core PRIVATE core/src/stream_session.cpp core/src/rtc_call.cpp core/src/signal_socket.cpp
        core/src/desktop_input.cpp core/src/cursor_shape.cpp core/src/mf_h264.cpp core/src/d3d11_video.cpp
        core/src/dxgi_capturer.cpp core/src/latency_trace.cpp)
    target_sources(core PRIVATE core/src/connection_options.cpp)
    target_link_libraries(core PUBLIC input_protocol PRIVATE WebRTC::WebRTC mfplat mfuuid dxguid delayimp)
    target_link_options(core INTERFACE /DELAYLOAD:mfplat.dll)
    set_source_files_properties(core/src/stream_session.cpp core/src/rtc_call.cpp core/src/mf_h264.cpp core/src/d3d11_video.cpp
        PROPERTIES COMPILE_OPTIONS "/GR-;/Zc:__cplusplus")

    add_library(stream_window STATIC viewer/stream_window.cpp viewer/input_test_pad.cpp)
    target_link_libraries(stream_window PUBLIC core PRIVATE comdlg32 shell32 uxtheme)
    target_compile_options(stream_window PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(p2premote_host PRIVATE stream_window)
    target_compile_definitions(p2premote_host PRIVATE P2PREMOTE_BUILD_STREAMING)

    add_executable(p2premote_viewer WIN32 viewer/main.cpp)
    target_link_libraries(p2premote_viewer PRIVATE stream_window)
    target_compile_options(p2premote_viewer PRIVATE /W4 /permissive- /utf-8)
    list(APPEND gui_targets p2premote_viewer)

    add_executable(p2premote WIN32 viewer/account_main.cpp viewer/account_api.cpp)
    include(cmake/curl-target.cmake)
    target_link_libraries(p2premote PRIVATE stream_window advapi32 windowsapp CURL::libcurl)
    target_compile_options(p2premote PRIVATE /W4 /permissive- /utf-8)
    if(P2PREMOTE_MINIRTC_DIR)
        foreach(file minirtc.h p2premote_minirtc.dll)
            if(NOT EXISTS "${P2PREMOTE_MINIRTC_DIR}/${file}")
                message(FATAL_ERROR "Build MiniRTC first with tools/minirtc_probe/Build.ps1. Missing ${file}")
            endif()
        endforeach()
        target_sources(core PRIVATE core/src/minirtc_call.cpp)
        target_compile_definitions(core PRIVATE P2PREMOTE_HAVE_MINIRTC)
        target_include_directories(core PRIVATE "${P2PREMOTE_MINIRTC_DIR}" tools/minirtc_probe)
        set_source_files_properties(core/src/minirtc_call.cpp PROPERTIES COMPILE_OPTIONS "/GR-;/Zc:__cplusplus")
        add_custom_target(p2premote_minirtc_runtime
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${P2PREMOTE_MINIRTC_DIR}/p2premote_minirtc.dll" "$<TARGET_FILE_DIR:p2premote>"
            VERBATIM)
        add_dependencies(p2premote p2premote_minirtc_runtime)
    endif()
    list(APPEND gui_targets p2premote)
endif()

if(P2PREMOTE_BUILD_WEBRTC_PROBE)
    enable_testing()
    add_subdirectory(tools/webrtc_probe)
    if(P2PREMOTE_BUILD_STREAMING)
        add_executable(codec_probe tools/codec_probe/main.cpp)
        target_include_directories(codec_probe PRIVATE core/src)
        target_link_libraries(codec_probe PRIVATE core WebRTC::WebRTC)
        target_compile_options(codec_probe PRIVATE /W4 /permissive- /utf-8 /GR- /Zc:__cplusplus)

        add_executable(input_protocol_test proto/input_protocol_test.cpp)
        target_link_libraries(input_protocol_test PRIVATE input_protocol)
        target_compile_options(input_protocol_test PRIVATE /W4 /permissive- /utf-8)
        add_test(NAME input_protocol_test COMMAND input_protocol_test)
    endif()
endif()

foreach(target IN LISTS gui_targets)
    target_link_options(${target} PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")
endforeach()
