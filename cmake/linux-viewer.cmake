find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network WebSockets OpenGLWidgets)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LINUX_LUA REQUIRED IMPORTED_TARGET lua5.4)
pkg_check_modules(LINUX_SECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(LINUX_PULSE REQUIRED IMPORTED_TARGET libpulse)
pkg_check_modules(LINUX_FUSE3 REQUIRED IMPORTED_TARGET fuse3)
find_path(LIBYUV_INCLUDE_DIR libyuv.h REQUIRED)
find_library(LIBYUV_LIBRARY NAMES yuv REQUIRED)
find_package(ghostty-vt 1.0.1 CONFIG REQUIRED
    HINTS "$ENV{HOME}/.local/share/p2premote/ghostty-vt")

set(P2PREMOTE_LINUX_MINIRTC_DIR "$ENV{HOME}/.local/share/p2premote/minirtc" CACHE PATH
    "Directory containing the Linux MiniRTC headers and library")
find_path(P2PREMOTE_LINUX_MINIRTC_INCLUDE minirtc.h
    PATHS "${P2PREMOTE_LINUX_MINIRTC_DIR}/include" NO_DEFAULT_PATH)
find_library(P2PREMOTE_LINUX_MINIRTC_LIBRARY NAMES p2premote_minirtc
    PATHS "${P2PREMOTE_LINUX_MINIRTC_DIR}/lib" NO_DEFAULT_PATH)
if(NOT P2PREMOTE_LINUX_MINIRTC_INCLUDE OR NOT P2PREMOTE_LINUX_MINIRTC_LIBRARY)
    message(FATAL_ERROR "Build the Linux MiniRTC library with bash tools/minirtc_probe/Build-Linux.sh first.")
endif()

add_executable(p2premote_linux_viewer
    viewer/qt/main.cpp
    viewer/qt/account_window.cpp
    viewer/qt/agent_broker.cpp
    viewer/qt/app_settings.cpp
    viewer/qt/app_theme.cpp
    viewer/qt/frameless_chrome.cpp
    viewer/qt/plugin_dialog.cpp
    viewer/qt/plugin_menu.cpp
    viewer/qt/remote_window.cpp
    viewer/qt/settings_dialog.cpp
    viewer/qt/stream_driver.cpp
    viewer/qt/terminal_widget.cpp
    viewer/linux/account.cpp
    viewer/linux/audio.cpp
    viewer/linux/credentials.cpp
    viewer/linux/cursor_shape.cpp
    viewer/linux/file_clipboard.cpp
    viewer/linux/lan_discovery.cpp
    viewer/linux/plugin_platform.cpp
    viewer/linux/single_instance.cpp
    viewer/linux/stream.cpp
    viewer/linux/video_surface.cpp
    viewer/linux/viewer_session.cpp
    core/src/plugin_host.cpp
    proto/input_protocol.cpp
    proto/plugin_protocol.cpp)
set_target_properties(p2premote_linux_viewer PROPERTIES AUTOMOC ON)
target_compile_features(p2premote_linux_viewer PRIVATE cxx_std_20)
target_compile_options(p2premote_linux_viewer PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(p2premote_linux_viewer PRIVATE
    proto/include core/include core/src tools/minirtc_probe viewer/linux)
target_include_directories(p2premote_linux_viewer SYSTEM PRIVATE
    "${P2PREMOTE_LINUX_MINIRTC_INCLUDE}" "${LIBYUV_INCLUDE_DIR}")
target_link_libraries(p2premote_linux_viewer PRIVATE Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::OpenGLWidgets OpenSSL::Crypto
    PkgConfig::LINUX_LUA PkgConfig::LINUX_SECRET PkgConfig::LINUX_PULSE PkgConfig::LINUX_FUSE3 ghostty-vt::ghostty-vt
    "${P2PREMOTE_LINUX_MINIRTC_LIBRARY}" "${LIBYUV_LIBRARY}")

add_custom_target(p2premote_linux_plugins
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/plugins"
        "$<TARGET_FILE_DIR:p2premote_linux_viewer>/plugins"
    VERBATIM)
add_dependencies(p2premote_linux_viewer p2premote_linux_plugins)

enable_testing()
foreach(protocol IN ITEMS input plugin)
    add_executable(${protocol}_protocol_test
        "proto/${protocol}_protocol.cpp" "proto/${protocol}_protocol_test.cpp")
    target_compile_features(${protocol}_protocol_test PRIVATE cxx_std_20)
    target_include_directories(${protocol}_protocol_test PRIVATE proto/include)
    add_test(NAME ${protocol}_protocol_test COMMAND ${protocol}_protocol_test)
endforeach()

add_executable(terminal_widget_test viewer/qt/terminal_widget_test.cpp viewer/qt/terminal_widget.cpp)
set_target_properties(terminal_widget_test PROPERTIES AUTOMOC ON)
target_compile_features(terminal_widget_test PRIVATE cxx_std_20)
target_link_libraries(terminal_widget_test PRIVATE Qt6::Widgets ghostty-vt::ghostty-vt)
add_test(NAME terminal_widget_test COMMAND terminal_widget_test)
set_tests_properties(terminal_widget_test PROPERTIES TIMEOUT 10 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(linux_cursor_shape_test viewer/linux/cursor_shape_test.cpp viewer/linux/cursor_shape.cpp)
target_compile_features(linux_cursor_shape_test PRIVATE cxx_std_20)
target_compile_options(linux_cursor_shape_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(linux_cursor_shape_test PRIVATE Qt6::Gui)
add_test(NAME linux_cursor_shape_test COMMAND linux_cursor_shape_test)
set_tests_properties(linux_cursor_shape_test PROPERTIES TIMEOUT 10)

add_executable(linux_audio_buffer_test viewer/linux/audio_buffer_test.cpp)
target_compile_features(linux_audio_buffer_test PRIVATE cxx_std_20)
target_compile_options(linux_audio_buffer_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME linux_audio_buffer_test COMMAND linux_audio_buffer_test)
set_tests_properties(linux_audio_buffer_test PROPERTIES TIMEOUT 10)

add_executable(linux_file_clipboard_test
    viewer/linux/file_clipboard_test.cpp viewer/linux/file_clipboard.cpp viewer/linux/plugin_platform.cpp
    core/src/plugin_host.cpp proto/plugin_protocol.cpp)
target_compile_features(linux_file_clipboard_test PRIVATE cxx_std_20)
target_compile_options(linux_file_clipboard_test PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(linux_file_clipboard_test PRIVATE core/include core/src proto/include viewer/linux)
target_link_libraries(linux_file_clipboard_test PRIVATE Qt6::Widgets OpenSSL::Crypto PkgConfig::LINUX_LUA PkgConfig::LINUX_FUSE3)
add_test(NAME linux_file_clipboard_test COMMAND linux_file_clipboard_test "${CMAKE_CURRENT_SOURCE_DIR}/plugins/file-clipboard")
set_tests_properties(linux_file_clipboard_test PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
