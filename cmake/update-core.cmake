find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(P2PREMOTE_UPDATE_TARGET "linux-x64")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(P2PREMOTE_UPDATE_TARGET "darwin-arm64")
    else()
        set(P2PREMOTE_UPDATE_TARGET "darwin-x64")
    endif()
else()
    set(P2PREMOTE_UPDATE_TARGET "windows-x64")
endif()

set(P2PREMOTE_UPDATE_FEED_URL
    "https://github.com/qisewai/p2premote-linux/releases/latest/download/update-${P2PREMOTE_UPDATE_TARGET}.json"
    CACHE STRING "Update manifest feed URL for this target platform")

set(P2PREMOTE_UPDATE_PUBLIC_KEY "" CACHE STRING
    "Base64-encoded 32-byte Ed25519 public key used to verify signed updates")
if(NOT P2PREMOTE_UPDATE_PUBLIC_KEY AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/update-public.key")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/update-public.key" P2PREMOTE_UPDATE_PUBLIC_KEY)
    string(STRIP "${P2PREMOTE_UPDATE_PUBLIC_KEY}" P2PREMOTE_UPDATE_PUBLIC_KEY)
endif()

add_library(p2premote_update_core STATIC
    core/update/ed25519.cpp
    core/update/ed25519.h
    core/update/update_archive.cpp
    core/update/update_archive.h
    core/update/update_delta.cpp
    core/update/update_delta.h)
target_compile_features(p2premote_update_core PUBLIC cxx_std_20)
target_include_directories(p2premote_update_core PUBLIC core/update)
target_compile_definitions(p2premote_update_core PUBLIC
    P2PREMOTE_VERSION="${PROJECT_VERSION}"
    P2PREMOTE_UPDATE_TARGET="${P2PREMOTE_UPDATE_TARGET}"
    P2PREMOTE_UPDATE_FEED_URL="${P2PREMOTE_UPDATE_FEED_URL}"
    P2PREMOTE_UPDATE_PUBLIC_KEY="${P2PREMOTE_UPDATE_PUBLIC_KEY}")
target_link_libraries(p2premote_update_core PUBLIC ZLIB::ZLIB BZip2::BZip2)
if(WIN32)
    set_target_properties(p2premote_update_core PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_definitions(p2premote_update_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(p2premote_update_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(p2premote_update_core PUBLIC bcrypt)
else()
    target_compile_options(p2premote_update_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(p2prelease tools/p2prelease/main.cpp)
target_link_libraries(p2prelease PRIVATE p2premote_update_core)
if(WIN32)
    set_target_properties(p2prelease PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_options(p2prelease PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(p2prelease PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_executable(update_core_test core/update/update_core_test.cpp)
target_link_libraries(update_core_test PRIVATE p2premote_update_core ZLIB::ZLIB)
if(WIN32)
    set_target_properties(update_core_test PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_options(update_core_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(update_core_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME update_core_test COMMAND update_core_test)
set_tests_properties(update_core_test PROPERTIES TIMEOUT 15)
