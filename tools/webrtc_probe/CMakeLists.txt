add_executable(p2premote_webrtc_probe main.cpp)
target_include_directories(p2premote_webrtc_probe PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
target_compile_features(p2premote_webrtc_probe PRIVATE cxx_std_20)
target_compile_options(p2premote_webrtc_probe PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /GR-)
target_link_libraries(p2premote_webrtc_probe PRIVATE WebRTC::WebRTC)

add_test(NAME webrtc_dependency_probe COMMAND p2premote_webrtc_probe)
set_tests_properties(webrtc_dependency_probe PROPERTIES TIMEOUT 30)
