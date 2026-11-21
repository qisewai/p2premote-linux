cmake_minimum_required(VERSION 3.23)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/webrtc-release.cmake")

file(MAKE_DIRECTORY "${P2PREMOTE_WEBRTC_CACHE}")
file(LOCK "${P2PREMOTE_WEBRTC_CACHE}/webrtc.lock" GUARD PROCESS TIMEOUT 15)

set(stamp "${P2PREMOTE_WEBRTC_DIR}/.archive-sha256")
if(EXISTS "${stamp}" AND EXISTS "${P2PREMOTE_WEBRTC_DIR}/lib/webrtc.lib"
        AND EXISTS "${P2PREMOTE_WEBRTC_DIR}/include/api/peer_connection_interface.h")
    file(READ "${stamp}" digest)
    if(digest STREQUAL P2PREMOTE_WEBRTC_SHA256)
        message(STATUS "WebRTC ${P2PREMOTE_WEBRTC_VERSION} is ready at ${P2PREMOTE_WEBRTC_DIR}")
        return()
    endif()
endif()

if(EXISTS "${P2PREMOTE_WEBRTC_ARCHIVE}")
    file(SHA256 "${P2PREMOTE_WEBRTC_ARCHIVE}" digest)
else()
    set(digest "")
endif()
if(NOT digest STREQUAL P2PREMOTE_WEBRTC_SHA256)
    message(STATUS "Downloading WebRTC ${P2PREMOTE_WEBRTC_VERSION} for Windows x64 (782 MB)")
    find_program(curl NAMES curl.exe REQUIRED)
    set(download "${P2PREMOTE_WEBRTC_ARCHIVE}.download")
    execute_process(COMMAND "${curl}" --fail --location --retry 2 --connect-timeout 15
        --speed-limit 1024 --speed-time 180 --silent --show-error
        --output "${download}" "${P2PREMOTE_WEBRTC_URL}"
        RESULT_VARIABLE download_result)
    if(NOT download_result STREQUAL "0")
        file(REMOVE "${download}")
        message(FATAL_ERROR "WebRTC download failed (${download_result})")
    endif()
    file(SHA256 "${download}" digest)
    if(NOT digest STREQUAL P2PREMOTE_WEBRTC_SHA256)
        file(REMOVE "${download}")
        message(FATAL_ERROR "WebRTC download SHA-256 mismatch (${digest})")
    endif()
    file(RENAME "${download}" "${P2PREMOTE_WEBRTC_ARCHIVE}")
endif()

get_filename_component(extract_dir "${P2PREMOTE_WEBRTC_DIR}" DIRECTORY)
file(MAKE_DIRECTORY "${extract_dir}")
file(REMOVE "${stamp}")
message(STATUS "Extracting the verified WebRTC archive")
file(ARCHIVE_EXTRACT INPUT "${P2PREMOTE_WEBRTC_ARCHIVE}" DESTINATION "${extract_dir}")
file(STRINGS "${P2PREMOTE_WEBRTC_DIR}/VERSIONS" source_revision REGEX "^WEBRTC_COMMIT=")
if(NOT source_revision STREQUAL "WEBRTC_COMMIT=${P2PREMOTE_WEBRTC_COMMIT}"
        OR NOT EXISTS "${P2PREMOTE_WEBRTC_DIR}/lib/webrtc.lib"
        OR NOT EXISTS "${P2PREMOTE_WEBRTC_DIR}/include/api/peer_connection_interface.h")
    message(FATAL_ERROR "The WebRTC archive has an unexpected source revision or layout")
endif()
file(WRITE "${stamp}" "${P2PREMOTE_WEBRTC_SHA256}")
message(STATUS "WebRTC ${P2PREMOTE_WEBRTC_VERSION} is ready at ${P2PREMOTE_WEBRTC_DIR}")
