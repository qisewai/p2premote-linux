find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(PkgConfig REQUIRED)
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network WebSockets)
pkg_check_modules(LINUX_HOST_X11 REQUIRED IMPORTED_TARGET xcb xcb-shm xcb-xfixes xau)
pkg_check_modules(LINUX_HOST_AUDIO REQUIRED IMPORTED_TARGET libpulse libsystemd)
find_path(LIBYUV_INCLUDE_DIR libyuv.h REQUIRED)
find_library(LIBYUV_LIBRARY NAMES yuv REQUIRED)

set(P2PREMOTE_LINUX_MINIRTC_DIR "$ENV{HOME}/.local/share/p2premote/minirtc" CACHE PATH
    "Directory containing the Linux MiniRTC headers and library")
find_path(P2PREMOTE_LINUX_MINIRTC_INCLUDE minirtc.h
    PATHS "${P2PREMOTE_LINUX_MINIRTC_DIR}/include" NO_DEFAULT_PATH)
find_library(P2PREMOTE_LINUX_MINIRTC_LIBRARY NAMES p2premote_minirtc
    PATHS "${P2PREMOTE_LINUX_MINIRTC_DIR}/lib" NO_DEFAULT_PATH)
if(NOT P2PREMOTE_LINUX_MINIRTC_INCLUDE OR NOT P2PREMOTE_LINUX_MINIRTC_LIBRARY)
    message(FATAL_ERROR "Build the Linux MiniRTC library with bash tools/minirtc_probe/Build-Linux.sh first.")
endif()

qt6_add_executable(p2premote_linux_host
    host/linux/main.cpp
    host/linux/clipboard_endpoint.cpp
    host/linux/clipboard_relay.cpp
    proto/plugin_protocol.cpp
    host/linux/cloud_host.cpp
    host/linux/desktop_audio.cpp
    host/linux/x11_shm_capturer.cpp
    host/linux/uinput_injector.cpp
    host/linux/lan_signal_server.cpp
    host/linux/host_session.cpp
    host/linux/service_support.cpp
    viewer/linux/cloud_connection.cpp
    proto/input_protocol.cpp)
target_compile_definitions(p2premote_linux_host PRIVATE
    P2PREMOTE_INSTALLED_GUI="${CMAKE_INSTALL_FULL_LIBEXECDIR}/p2premote/gui/p2premote")
target_compile_features(p2premote_linux_host PRIVATE cxx_std_20)
target_compile_options(p2premote_linux_host PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(p2premote_linux_host PRIVATE
    proto/include core/include core/src tools/minirtc_probe host/linux viewer/linux)
target_include_directories(p2premote_linux_host SYSTEM PRIVATE
    "${P2PREMOTE_LINUX_MINIRTC_INCLUDE}" "${LIBYUV_INCLUDE_DIR}")
target_link_libraries(p2premote_linux_host PRIVATE
    Qt6::Core Qt6::Network Qt6::WebSockets OpenSSL::Crypto PkgConfig::LINUX_HOST_X11 PkgConfig::LINUX_HOST_AUDIO pthread
    "${P2PREMOTE_LINUX_MINIRTC_LIBRARY}" "${LIBYUV_LIBRARY}")
set_target_properties(p2premote_linux_host PROPERTIES
    BUILD_RPATH "\$ORIGIN/lib"
    INSTALL_RPATH "\$ORIGIN/lib")
add_custom_command(TARGET p2premote_linux_host POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:p2premote_linux_host>/lib"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${P2PREMOTE_LINUX_MINIRTC_LIBRARY}"
        "$<TARGET_FILE_DIR:p2premote_linux_host>/lib/"
    VERBATIM)

set(P2PREMOTE_LINUX_HOST_EXECUTABLE "${CMAKE_INSTALL_FULL_LIBEXECDIR}/p2premote/p2premote_linux_host")
configure_file(host/linux/p2premote-host.service.in p2premote-host.service @ONLY)
install(TARGETS p2premote_linux_host RUNTIME DESTINATION "${P2PREMOTE_LINUX_APP_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/p2premote-host.service" DESTINATION "${P2PREMOTE_LINUX_APP_DIR}/units")
if(QT6_IS_SHARED_LIBS_BUILD AND NOT CMAKE_CROSSCOMPILING)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/p2premote-host-qt.conf" "[Paths]\nPrefix=.\nLibraries=lib\nPlugins=qt-plugins\n")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/p2premote-host-qt.conf" DESTINATION "${P2PREMOTE_LINUX_APP_DIR}" RENAME qt.conf)
    qt6_generate_deploy_script(TARGET p2premote_linux_host OUTPUT_SCRIPT p2premote_host_qt_deploy CONTENT "
qt6_deploy_runtime_dependencies(
    EXECUTABLE [[$<TARGET_FILE:p2premote_linux_host>]]
    BIN_DIR [[${P2PREMOTE_LINUX_APP_DIR}]]
    LIB_DIR [[${P2PREMOTE_LINUX_APP_DIR}/lib]]
    PLUGINS_DIR [[${P2PREMOTE_LINUX_APP_DIR}/qt-plugins]]
    NO_TRANSLATIONS
    PRE_EXCLUDE_REGEXES [[^libp2premote_minirtc[.]so$]])
")
    install(SCRIPT "${p2premote_host_qt_deploy}")
endif()

pkg_check_modules(LINUX_LUA REQUIRED IMPORTED_TARGET lua5.4)
pkg_check_modules(LINUX_FUSE3 REQUIRED IMPORTED_TARGET fuse3)
add_library(linux_host_clipboard STATIC
    host/linux/clipboard_session.cpp host/linux/clipboard_endpoint.cpp
    viewer/linux/file_clipboard.cpp viewer/linux/plugin_platform.cpp
    core/src/plugin_host.cpp core/src/pty_session_posix.cpp proto/plugin_protocol.cpp)
target_compile_features(linux_host_clipboard PUBLIC cxx_std_20)
target_compile_options(linux_host_clipboard PUBLIC -Wall -Wextra -Wpedantic)
target_compile_definitions(linux_host_clipboard PUBLIC P2PREMOTE_LINUX_PLUGIN_DIR="${CMAKE_INSTALL_FULL_DATADIR}/p2premote/plugins")
target_include_directories(linux_host_clipboard PUBLIC proto/include core/include core/src viewer/linux)
target_link_libraries(linux_host_clipboard PUBLIC Qt6::Widgets OpenSSL::Crypto PkgConfig::LINUX_LUA
    PkgConfig::LINUX_FUSE3 PkgConfig::LINUX_HOST_AUDIO util dl)
qt6_add_executable(p2premote_clipboard host/linux/clipboard_agent.cpp)
target_link_libraries(p2premote_clipboard PRIVATE linux_host_clipboard)
set_target_properties(p2premote_clipboard PROPERTIES INSTALL_RPATH "\$ORIGIN/lib")
add_dependencies(p2premote_linux_host p2premote_clipboard)
install(TARGETS p2premote_clipboard RUNTIME DESTINATION "${P2PREMOTE_LINUX_APP_DIR}")
if(NOT P2PREMOTE_BUILD_LINUX_VIEWER)
    install(DIRECTORY plugins/clipboard-sync plugins/file-clipboard plugins/file-transfer
        plugins/remote-terminal plugins/keyboard-sync
        DESTINATION "${CMAKE_INSTALL_DATADIR}/p2premote/plugins")
endif()
set(P2PREMOTE_XDG_AUTOSTART_DIR "/etc/xdg/autostart" CACHE PATH "System XDG desktop autostart directory")
configure_file(host/linux/clipboard.desktop.in p2premote-clipboard.desktop @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/p2premote-clipboard.desktop" DESTINATION "${P2PREMOTE_XDG_AUTOSTART_DIR}")
if(QT6_IS_SHARED_LIBS_BUILD AND NOT CMAKE_CROSSCOMPILING)
    qt6_generate_deploy_script(TARGET p2premote_clipboard OUTPUT_SCRIPT clipboard_deploy CONTENT "
qt6_deploy_runtime_dependencies(
    EXECUTABLE [[$<TARGET_FILE:p2premote_clipboard>]]
    BIN_DIR [[${P2PREMOTE_LINUX_APP_DIR}]]
    LIB_DIR [[${P2PREMOTE_LINUX_APP_DIR}/lib]]
    PLUGINS_DIR [[${P2PREMOTE_LINUX_APP_DIR}/qt-plugins]]
    NO_TRANSLATIONS)
")
    install(SCRIPT "${clipboard_deploy}")
endif()

enable_testing()
add_executable(linux_clipboard_session_test host/linux/clipboard_session_test.cpp)
target_link_libraries(linux_clipboard_session_test PRIVATE linux_host_clipboard)
add_custom_command(TARGET linux_clipboard_session_test POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/plugins/clipboard-sync"
        "$<TARGET_FILE_DIR:linux_clipboard_session_test>/plugins/clipboard-sync"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/plugins/file-clipboard"
        "$<TARGET_FILE_DIR:linux_clipboard_session_test>/plugins/file-clipboard" VERBATIM)
add_test(NAME linux_clipboard_session_test COMMAND linux_clipboard_session_test)
set_tests_properties(linux_clipboard_session_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 22)

add_executable(linux_host_test
    host/linux/linux_host_test.cpp
    host/linux/x11_shm_capturer.cpp
    host/linux/uinput_injector.cpp
    host/linux/lan_signal_server.cpp
    proto/input_protocol.cpp)
target_compile_features(linux_host_test PRIVATE cxx_std_20)
target_compile_options(linux_host_test PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(linux_host_test PRIVATE proto/include host/linux)
target_link_libraries(linux_host_test PRIVATE OpenSSL::Crypto PkgConfig::LINUX_HOST_X11 pthread)
add_test(NAME linux_host_test COMMAND linux_host_test)
set_tests_properties(linux_host_test PROPERTIES TIMEOUT 10)

add_executable(pty_session_posix_test
    core/src/pty_session_posix_test.cpp
    core/src/pty_session_posix.cpp)
target_compile_features(pty_session_posix_test PRIVATE cxx_std_20)
target_compile_options(pty_session_posix_test PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(pty_session_posix_test PRIVATE core/src)
target_link_libraries(pty_session_posix_test PRIVATE util pthread)
add_test(NAME pty_session_posix_test COMMAND pty_session_posix_test)
set_tests_properties(pty_session_posix_test PROPERTIES TIMEOUT 10)

find_program(P2PREMOTE_TEST_PYTHON python3)
if(P2PREMOTE_TEST_PYTHON)
    add_test(NAME linux_service_lifecycle_test COMMAND "${P2PREMOTE_TEST_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/host/linux/service_lifecycle_test.py" "$<TARGET_FILE:p2premote_linux_host>")
    set_tests_properties(linux_service_lifecycle_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 25)
endif()
