find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)

add_executable(p2premote_qt WIN32
    viewer/qt/main.cpp
    viewer/qt/account_window.cpp
    viewer/qt/credentials.cpp
    viewer/qt/remote_window.cpp
    viewer/qt/stream_driver.cpp
    viewer/qt/stream_driver.h)
set_target_properties(p2premote_qt PROPERTIES
    AUTOMOC ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_compile_features(p2premote_qt PRIVATE cxx_std_20)
target_compile_definitions(p2premote_qt PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(p2premote_qt PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(p2premote_qt PRIVATE p2premote_core Qt6::Widgets advapi32 windowsapp)
target_link_options(p2premote_qt PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")
set_property(TARGET p2premote_qt APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/dpi.manifest")

add_custom_command(TARGET p2premote_qt POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:p2premote_qt>/platforms"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>" "$<TARGET_FILE_DIR:p2premote_qt>/platforms"
    VERBATIM)

install(TARGETS p2premote_qt p2premote p2premote_core RUNTIME DESTINATION .)
install(FILES "${P2PREMOTE_MINIRTC_DIR}/p2premote_minirtc.dll" DESTINATION .)
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
    foreach(port qtbase double-conversion md4c pcre2 zlib)
        install(FILES "${vcpkg_triplet_dir}/share/${port}/copyright"
            DESTINATION licenses RENAME "${port}.txt" OPTIONAL)
    endforeach()
endif()
