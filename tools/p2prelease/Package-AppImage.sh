#!/bin/bash
set -euo pipefail

if (( $# < 2 || $# > 3 )); then
    echo "Usage: $0 <configured Linux x86_64 build directory> <output.AppImage> [appimagetool]" >&2
    exit 2
fi
if [[ $(uname -m) != x86_64 ]]; then
    echo "Build the Linux x86_64 AppImage on an x86_64 Linux system." >&2
    exit 1
fi
build_dir=$(realpath -- "$1")
output=$(realpath -m -- "$2")
script_dir=$(dirname -- "$(realpath -- "$0")")
appimagetool=$(command -v -- "${3:-appimagetool}")
command -v patchelf >/dev/null
command -v python3 >/dev/null
command -v openssl >/dev/null

work=$(mktemp -d)
app_dir="$work/P2PRemote.AppDir"
mkdir -p -- "$app_dir" "$(dirname -- "$output")"
build_output=$(mktemp --tmpdir="$(dirname -- "$output")" --suffix=.AppImage .p2premote-XXXXXXXX)
trap 'rm -rf -- "$work"; rm -f -- "$build_output"' EXIT
DESTDIR="$app_dir" cmake --install "$build_dir"
runtime="$app_dir/usr/local/libexec/p2premote"
if [[ ! -x "$runtime/gui/p2premote" || ! -x "$runtime/p2premote_linux_host" ]]; then
    echo "Configure and build the complete linux-x64 preset with CMAKE_INSTALL_PREFIX=/usr/local." >&2
    exit 1
fi
cp -a -- "$app_dir/usr/local/share/p2premote/plugins" "$runtime/gui/plugins"
python3 "$script_dir/Bundle-LinuxRuntime.py" "$runtime"
(
    cd "$app_dir"
    find usr etc -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1
) > "$work/appimage-build"
install -m 0644 -- "$work/appimage-build" "$runtime/.appimage-build"
install -m 0755 -- "$script_dir/AppRun.sh" "$app_dir/AppRun"
install -m 0644 -- "$script_dir/p2premote.svg" "$app_dir/p2premote.svg"
sed 's|^Exec=.*|Exec=p2premote|; s|^Icon=.*|Icon=p2premote|' \
    "$app_dir/usr/local/share/applications/p2premote.desktop" > "$app_dir/p2premote.desktop"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$appimagetool" --no-appstream "$app_dir" "$build_output"
mv -fT -- "$build_output" "$output"
