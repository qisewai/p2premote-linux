include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.4.9.tar.gz
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6)
FetchContent_MakeAvailable(lua)

file(GLOB lua_sources "${lua_SOURCE_DIR}/src/*.c")
list(FILTER lua_sources EXCLUDE REGEX "/(lua|luac|onelua|linit|loadlib|liolib|loslib|ldblib|lcorolib)\\.c$")
add_library(p2premote_lua STATIC ${lua_sources})
target_include_directories(p2premote_lua SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")
set(P2PREMOTE_LUA_NOTICE "${lua_SOURCE_DIR}/doc/readme.html")
