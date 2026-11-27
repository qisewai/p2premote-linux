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

qt6_add_executable(p2premote_linux_viewer
    viewer/qt/main.cpp
    viewer/qt/account_window.cpp
    viewer/qt/agent_broker.cpp
    viewer/qt/app_settings.cpp
    viewer/qt/app_theme.cpp
    viewer/qt/connection_stage_overlay.cpp
    viewer/qt/connection_stage_overlay.h
    viewer/qt/frameless_chrome.cpp
    viewer/qt/plugin_dialog.cpp
    viewer/qt/plugin_menu.cpp
    viewer/qt/remote_window.cpp
    viewer/qt/settings_dialog.cpp
    viewer/qt/stream_driver.cpp
    viewer/qt/terminal_widget.cpp
    viewer/qt/update_client.cpp
    viewer/qt/update_client.h
    viewer/linux/account.cpp
    viewer/linux/cloud_connection.cpp
    viewer/linux/audio.cpp
    viewer/linux/credentials.cpp
    viewer/linux/cursor_shape.cpp
    viewer/linux/file_clipboard.cpp
    viewer/linux/lan_discovery.cpp
    viewer/linux/plugin_platform.cpp
    viewer/linux/service_control.cpp
    viewer/linux/single_instance.cpp
    viewer/linux/stream.cpp
    viewer/linux/update_apply_linux.cpp
    viewer/linux/video_surface.cpp
    viewer/linux/viewer_session.cpp
    core/src/plugin_host.cpp
    core/src/pty_session_posix.cpp
    proto/input_protocol.cpp
    proto/plugin_protocol.cpp)
set_target_properties(p2premote_linux_viewer PROPERTIES AUTOMOC ON)
if(P2PREMOTE_BUILD_LINUX_HOST)
    set_target_properties(p2premote_linux_viewer PROPERTIES OUTPUT_NAME p2premote)
endif()
set(p2premote_gui_dir "${P2PREMOTE_LINUX_APP_DIR}/gui")
set_target_properties(p2premote_linux_viewer PROPERTIES INSTALL_RPATH "\$ORIGIN/lib;\$ORIGIN/../lib")
target_compile_definitions(p2premote_linux_viewer PRIVATE
    P2PREMOTE_LINUX_HOST_EXECUTABLE="${CMAKE_INSTALL_FULL_LIBEXECDIR}/p2premote/p2premote_linux_host"
    P2PREMOTE_LINUX_PLUGIN_DIR="${CMAKE_INSTALL_FULL_DATADIR}/p2premote/plugins")
target_compile_features(p2premote_linux_viewer PRIVATE cxx_std_20)
target_compile_options(p2premote_linux_viewer PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(p2premote_linux_viewer PRIVATE
    proto/include core/include core/src tools/minirtc_probe viewer/linux)
target_include_directories(p2premote_linux_viewer SYSTEM PRIVATE
    "${P2PREMOTE_LINUX_MINIRTC_INCLUDE}" "${LIBYUV_INCLUDE_DIR}")
target_link_libraries(p2premote_linux_viewer PRIVATE p2premote_update_core Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::OpenGLWidgets OpenSSL::Crypto
    PkgConfig::LINUX_LUA PkgConfig::LINUX_SECRET PkgConfig::LINUX_PULSE PkgConfig::LINUX_FUSE3 ghostty-vt::ghostty-vt
    "${P2PREMOTE_LINUX_MINIRTC_LIBRARY}" "${LIBYUV_LIBRARY}" util dl)
if(TARGET Qt6::QOffscreenIntegrationPlugin)
    qt6_import_plugins(p2premote_linux_viewer INCLUDE Qt6::QOffscreenIntegrationPlugin)
endif()
find_package(Qt6WaylandClient 6.8 QUIET CONFIG)
if(TARGET Qt6::QWaylandIntegrationPlugin AND TARGET Qt6::QWaylandXdgShellIntegrationPlugin)
    qt6_import_plugins(p2premote_linux_viewer INCLUDE
        Qt6::QWaylandIntegrationPlugin Qt6::QWaylandXdgShellIntegrationPlugin)
endif()

add_custom_target(p2premote_linux_plugins
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/plugins"
        "$<TARGET_FILE_DIR:p2premote_linux_viewer>/plugins"
    VERBATIM)
add_dependencies(p2premote_linux_viewer p2premote_linux_plugins)
get_target_property(P2PREMOTE_LINUX_CLIENT_NAME p2premote_linux_viewer OUTPUT_NAME)
if(NOT P2PREMOTE_LINUX_CLIENT_NAME)
    set(P2PREMOTE_LINUX_CLIENT_NAME p2premote_linux_viewer)
endif()
file(RELATIVE_PATH P2PREMOTE_LINUX_CLIENT_DIR "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBEXECDIR}/p2premote/gui")
configure_file(viewer/linux/launch-client.sh.in "${P2PREMOTE_LINUX_CLIENT_NAME}-launcher" @ONLY)
install(TARGETS p2premote_linux_viewer RUNTIME DESTINATION "${p2premote_gui_dir}")
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/${P2PREMOTE_LINUX_CLIENT_NAME}-launcher"
    DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME "${P2PREMOTE_LINUX_CLIENT_NAME}")
if(P2PREMOTE_BUILD_LINUX_HOST)
    configure_file(viewer/linux/p2premote.desktop.in p2premote.desktop @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/p2premote.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
endif()
install(DIRECTORY plugins/ DESTINATION "${CMAKE_INSTALL_DATADIR}/p2premote/plugins")
if(QT6_IS_SHARED_LIBS_BUILD AND NOT CMAKE_CROSSCOMPILING)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/p2premote-qt.conf" "[Paths]\nPrefix=.\nLibraries=lib\nPlugins=qt-plugins\n")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/p2premote-qt.conf" DESTINATION "${p2premote_gui_dir}" RENAME qt.conf)
    qt6_generate_deploy_script(TARGET p2premote_linux_viewer OUTPUT_SCRIPT p2premote_qt_deploy CONTENT "
qt6_deploy_runtime_dependencies(
    EXECUTABLE [[$<TARGET_FILE:p2premote_linux_viewer>]]
    BIN_DIR [[${p2premote_gui_dir}]]
    LIB_DIR [[${p2premote_gui_dir}/lib]]
    PLUGINS_DIR [[${p2premote_gui_dir}/qt-plugins]]
    NO_TRANSLATIONS
    PRE_EXCLUDE_REGEXES [[^libp2premote_minirtc[.]so$]])
")
    install(SCRIPT "${p2premote_qt_deploy}")
else()
    install(IMPORTED_RUNTIME_ARTIFACTS ghostty-vt::ghostty-vt LIBRARY DESTINATION "${p2premote_gui_dir}/lib")
endif()

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
    core/src/plugin_host.cpp core/src/pty_session_posix.cpp proto/plugin_protocol.cpp)
target_compile_features(linux_file_clipboard_test PRIVATE cxx_std_20)
target_compile_options(linux_file_clipboard_test PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(linux_file_clipboard_test PRIVATE core/include core/src proto/include viewer/linux)
target_link_libraries(linux_file_clipboard_test PRIVATE Qt6::Widgets OpenSSL::Crypto PkgConfig::LINUX_LUA PkgConfig::LINUX_FUSE3 util dl)
add_test(NAME linux_file_clipboard_test COMMAND linux_file_clipboard_test "${CMAKE_CURRENT_SOURCE_DIR}/plugins/file-clipboard")
set_tests_properties(linux_file_clipboard_test PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(update_client_test viewer/qt/update_client_test.cpp viewer/qt/update_client.cpp viewer/qt/update_client.h)
set_target_properties(update_client_test PROPERTIES AUTOMOC ON)
target_compile_features(update_client_test PRIVATE cxx_std_20)
target_compile_options(update_client_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(update_client_test PRIVATE p2premote_update_core Qt6::Network)
add_test(NAME update_client_test COMMAND update_client_test)
set_tests_properties(update_client_test PROPERTIES TIMEOUT 15)

add_executable(connection_stage_overlay_test
    viewer/qt/connection_stage_overlay_test.cpp
    viewer/qt/connection_stage_overlay.cpp
    viewer/qt/connection_stage_overlay.h
    viewer/qt/app_theme.cpp
    viewer/qt/app_theme.h)
set_target_properties(connection_stage_overlay_test PROPERTIES AUTOMOC ON)
target_compile_features(connection_stage_overlay_test PRIVATE cxx_std_20)
target_compile_options(connection_stage_overlay_test PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(connection_stage_overlay_test PRIVATE core/include)
target_link_libraries(connection_stage_overlay_test PRIVATE Qt6::Widgets)
add_test(NAME connection_stage_overlay_test COMMAND connection_stage_overlay_test)
set_tests_properties(connection_stage_overlay_test PROPERTIES TIMEOUT 10 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
