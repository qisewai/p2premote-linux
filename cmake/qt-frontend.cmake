find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network)

set(P2PREMOTE_GHOSTTY_VT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/out/ghostty-vt/ghostty-vt-1.0.1-x86_64-windows-gnu"
    CACHE PATH "Directory containing libghostty-vt release (bin/ghostty-vt.dll, include/, lib/cmake/ghostty-vt/)")
set(ghostty-vt_DIR "${P2PREMOTE_GHOSTTY_VT_DIR}/lib/cmake/ghostty-vt")
find_package(ghostty-vt 1.0.1 CONFIG QUIET NO_DEFAULT_PATH)
if(NOT TARGET ghostty-vt::ghostty-vt)
    message(FATAL_ERROR
        "Qt requires the libghostty-vt 1.0.1 Windows x64 package. See README.md (Qt 6) "
        "and set P2PREMOTE_GHOSTTY_VT_DIR to the extracted package root. "
        "Current path: ${P2PREMOTE_GHOSTTY_VT_DIR}")
endif()

add_executable(p2premote_qt WIN32
    agent/snapshot.cpp
    viewer/qt/main.cpp
    viewer/qt/account_window.cpp
    viewer/qt/agent_broker.cpp
    viewer/qt/agent_broker.h
    viewer/qt/app_settings.cpp
    viewer/qt/app_settings.h
    viewer/qt/app_theme.cpp
    viewer/qt/app_theme.h
    viewer/qt/credentials.cpp
    viewer/qt/frameless_chrome.cpp
    viewer/qt/frameless_chrome.h
    viewer/qt/plugin_dialog.cpp
    viewer/qt/plugin_dialog.h
    viewer/qt/plugin_menu.cpp
    viewer/qt/plugin_menu.h
    viewer/qt/remote_window.cpp
    viewer/qt/remote_window.h
    viewer/connection_overlay.cpp
    viewer/qt/service_control.cpp
    viewer/qt/service_control.h
    viewer/qt/single_instance.cpp
    viewer/qt/settings_dialog.cpp
    viewer/qt/settings_dialog.h
    viewer/qt/stream_driver.cpp
    viewer/qt/stream_driver.h
    viewer/qt/terminal_widget.cpp
    viewer/qt/terminal_widget.h
    viewer/qt/update_client.cpp
    viewer/qt/update_client.h
    viewer/qt/update_apply_win.cpp)
set_target_properties(p2premote_qt PROPERTIES
    AUTOMOC ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_compile_features(p2premote_qt PRIVATE cxx_std_20)
target_compile_definitions(p2premote_qt PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(p2premote_qt PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(p2premote_qt PRIVATE p2premote_core p2premote_update_core Qt6::Widgets Qt6::Network ghostty-vt::ghostty-vt advapi32 crypt32 dwmapi shell32 windowsapp)
target_link_options(p2premote_qt PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")
set_property(TARGET p2premote_qt APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")

add_custom_command(TARGET p2premote_qt POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:p2premote_qt>/platforms"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>" "$<TARGET_FILE_DIR:p2premote_qt>/platforms"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:ghostty-vt::ghostty-vt>" "$<TARGET_FILE_DIR:p2premote_qt>"
    VERBATIM)

add_executable(terminal_widget_test viewer/qt/terminal_widget_test.cpp
    viewer/qt/terminal_widget.cpp viewer/qt/terminal_widget.h)
set_target_properties(terminal_widget_test PROPERTIES
    AUTOMOC ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_compile_features(terminal_widget_test PRIVATE cxx_std_20)
target_compile_options(terminal_widget_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(terminal_widget_test PRIVATE Qt6::Widgets ghostty-vt::ghostty-vt)
add_dependencies(terminal_widget_test p2premote_qt)
add_test(NAME terminal_widget_test COMMAND terminal_widget_test)
set_tests_properties(terminal_widget_test PROPERTIES TIMEOUT 10)

add_executable(agent_broker_test viewer/qt/agent_broker_test.cpp viewer/qt/agent_broker.cpp agent/snapshot.cpp)
set_target_properties(agent_broker_test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_compile_features(agent_broker_test PRIVATE cxx_std_20)
target_compile_definitions(agent_broker_test PRIVATE P2PREMOTE_CORE_EXPORTS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(agent_broker_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_include_directories(agent_broker_test PRIVATE core/include)
target_link_libraries(agent_broker_test PRIVATE Qt6::Network windowsapp)
add_dependencies(agent_broker_test p2p_cli p2premote_mcp)
add_test(NAME agent_broker_test COMMAND agent_broker_test "$<TARGET_FILE:p2p_cli>" "$<TARGET_FILE:p2premote_mcp>")
set_tests_properties(agent_broker_test PROPERTIES TIMEOUT 30)

add_executable(update_client_test viewer/qt/update_client_test.cpp viewer/qt/update_client.cpp viewer/qt/update_client.h)
set_target_properties(update_client_test PROPERTIES AUTOMOC ON MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_compile_features(update_client_test PRIVATE cxx_std_20)
target_compile_definitions(update_client_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(update_client_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(update_client_test PRIVATE p2premote_update_core Qt6::Network)
add_test(NAME update_client_test COMMAND update_client_test)
set_tests_properties(update_client_test PROPERTIES TIMEOUT 15)

install(TARGETS p2premote_qt p2premote p2premote_service p2premote_core p2premote_mcp p2p_cli RUNTIME DESTINATION .)
install(FILES "${P2PREMOTE_MINIRTC_DIR}/p2premote_minirtc.dll" DESTINATION .)
install(FILES "$<TARGET_FILE:ghostty-vt::ghostty-vt>" DESTINATION .)
get_target_property(ghostty_vt_dll ghostty-vt::ghostty-vt IMPORTED_LOCATION)
get_filename_component(ghostty_vt_bin_dir "${ghostty_vt_dll}" DIRECTORY)
install(FILES "${ghostty_vt_bin_dir}/../LICENSE" DESTINATION licenses RENAME ghostty-vt.txt)
install(FILES "${P2PREMOTE_LUA_NOTICE}" DESTINATION . RENAME lua-notice.html)
foreach(plugin IN LISTS P2PREMOTE_BUILTIN_PLUGINS)
    install(FILES "plugins/${plugin}/plugin.json" "plugins/${plugin}/main.lua" DESTINATION "plugins/${plugin}")
endforeach()

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
include(InstallRequiredSystemLibraries)
qt_generate_deploy_app_script(TARGET p2premote_qt OUTPUT_SCRIPT qt_deploy
    DEPLOY_TOOL_OPTIONS --no-compiler-runtime --no-translations --no-opengl-sw --no-system-dxc-compiler
        --skip-plugin-types generic,imageformats,networkinformation,styles,tls)
install(CODE [[
    set(QT_DEPLOY_BIN_DIR ".")
    set(QT_DEPLOY_PLUGINS_DIR ".")
]])
install(SCRIPT "${qt_deploy}")

if(VCPKG_TOOLCHAIN)
    set(vcpkg_triplet_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    # windeployqt handles Qt modules and plugins. vcpkg supplies their imported dependencies.
    install(CODE "
        execute_process(COMMAND \"${Z_VCPKG_EXECUTABLE}\" z-applocal
            \"--target-binary=\${CMAKE_INSTALL_PREFIX}/p2premote_qt.exe\"
            \"--installed-bin-dir=${vcpkg_triplet_dir}/bin\"
            COMMAND_ERROR_IS_FATAL ANY)
    ")
    foreach(port qtbase double-conversion md4c pcre2 zlib bzip2)
        install(FILES "${vcpkg_triplet_dir}/share/${port}/copyright"
            DESTINATION licenses RENAME "${port}.txt" OPTIONAL)
    endforeach()
endif()
