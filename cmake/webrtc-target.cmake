include("${CMAKE_CURRENT_LIST_DIR}/webrtc-release.cmake")

if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "The pinned WebRTC archive is for Windows x64")
endif()
if((CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_CONFIGURATION_TYPES STREQUAL "Release")
        OR (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE STREQUAL "Release"))
    message(FATAL_ERROR "The pinned WebRTC archive is a Release build. Use the windows-x64-webrtc preset.")
endif()
if(NOT EXISTS "${P2PREMOTE_WEBRTC_DIR}/.archive-sha256")
    message(FATAL_ERROR "Run cmake -P tools/fetch_webrtc.cmake to install the pinned WebRTC archive.")
endif()
file(READ "${P2PREMOTE_WEBRTC_DIR}/.archive-sha256" archive_digest)
if(NOT archive_digest STREQUAL P2PREMOTE_WEBRTC_SHA256)
    message(FATAL_ERROR "The extracted WebRTC archive has an unexpected hash. Run cmake -P tools/fetch_webrtc.cmake.")
endif()

add_library(WebRTC::WebRTC STATIC IMPORTED)
set_target_properties(WebRTC::WebRTC PROPERTIES
    IMPORTED_LOCATION "${P2PREMOTE_WEBRTC_DIR}/lib/webrtc.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${P2PREMOTE_WEBRTC_DIR}/include;${P2PREMOTE_WEBRTC_DIR}/include/third_party/abseil-cpp;${P2PREMOTE_WEBRTC_DIR}/include/third_party/libyuv/include"
    INTERFACE_COMPILE_DEFINITIONS "WEBRTC_WIN;NOMINMAX;WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0A00"
)
target_link_libraries(WebRTC::WebRTC INTERFACE
    advapi32 avrt bcrypt crypt32 dmoguids dwmapi iphlpapi msdmo ntdll ole32 oleaut32
    secur32 strmiids userenv uuid winmm wmcodecdspuuid ws2_32
)
