# GitHub release asset metadata for Shiguredo's Windows x64 build.
set(P2PREMOTE_WEBRTC_VERSION "m156.8078.3.4")
set(P2PREMOTE_WEBRTC_COMMIT "3987dfb7b952b5c86cb7d93370d48416a81d6397")
set(P2PREMOTE_WEBRTC_SHA256 "1376a6bbcaed7ed881f17512cb499924177a60c1a2ce967800d1eeabcfd02df8")
set(P2PREMOTE_WEBRTC_URL "https://github.com/shiguredo-webrtc-build/webrtc-build/releases/download/${P2PREMOTE_WEBRTC_VERSION}/webrtc.windows_x86_64.zip")

get_filename_component(P2PREMOTE_WEBRTC_CACHE "${CMAKE_CURRENT_LIST_DIR}/../out/deps" ABSOLUTE)
set(P2PREMOTE_WEBRTC_ARCHIVE "${P2PREMOTE_WEBRTC_CACHE}/webrtc.${P2PREMOTE_WEBRTC_VERSION}.windows_x86_64.zip")
set(P2PREMOTE_WEBRTC_DIR "${P2PREMOTE_WEBRTC_CACHE}/${P2PREMOTE_WEBRTC_VERSION}/webrtc")
