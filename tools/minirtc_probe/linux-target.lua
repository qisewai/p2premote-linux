target("p2premote_minirtc")
    set_kind("shared")
    set_default(false)
    set_targetdir(os.getenv("P2PREMOTE_LINUX_MINIRTC_OUTPUT"))
    add_deps("minirtc")
    add_packages("spdlog", "libyuv")
    add_syslinks("avcodec", "avutil")
    -- Hide bundled OpenSSL and codec symbols from Qt and other system libraries.
    add_shflags("-Wl,--version-script=" .. path.join(os.scriptdir(), "linux-exports.map"), {force = true})
    add_files(path.join(os.scriptdir(), "managed_dll.cpp"), path.join(os.scriptdir(), "linux_h264.cpp"))
    add_includedirs(
        path.join(os.projectdir(), "src/rtc"),
        path.join(os.projectdir(), "src/api"),
        path.join(os.projectdir(), "src/media"),
        path.join(os.projectdir(), "src/frame"),
        path.join(os.projectdir(), "src/common"),
        path.join(os.projectdir(), "src/qos"),
        path.join(os.projectdir(), "src/log"))
target_end()
