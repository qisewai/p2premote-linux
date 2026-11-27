#!/usr/bin/env bash
set -euo pipefail

tools_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$tools_dir/../.." && pwd)
revision=f5b53e920db0713b456e23af6a0fe9907d94a612
source_dir="${XDG_CACHE_HOME:-$HOME/.cache}/p2premote/minirtc-${revision:0:7}-linux"
prefix="${P2PREMOTE_LINUX_MINIRTC_DIR:-$HOME/.local/share/p2premote/minirtc}"
xmake="${XMAKE_BIN:-xmake}"
patches=(
    "$tools_dir/ExternalSignaling.patch"
    "$tools_dir/ManagedStats.patch"
    "$tools_dir/LowLatency.patch"
    "$tools_dir/DesktopVideoQuality.patch"
    "$tools_dir/DesktopIdleResume.patch"
    "$tools_dir/RecoveredPacketOrdering.patch"
    "$tools_dir/TrafficStats.patch"
    "$tools_dir/AudioQuality.patch"
    "$tools_dir/FastIceMediaReady.patch"
    "$tools_dir/LinuxFastIce.patch"
)

mkdir -p -- "$source_dir" "$prefix/include" "$prefix/lib"
if [[ ! -d "$source_dir/.git" ]]; then
    git -C "$source_dir" init --quiet
fi
if ! git -C "$source_dir" rev-parse --verify HEAD >/dev/null 2>&1; then
    seed="$repo/out/minirtc-deps/minirtc-${revision:0:7}"
    if git -c "safe.directory=$seed" -C "$seed" cat-file -e "$revision^{commit}" 2>/dev/null; then
        git -C "$source_dir" fetch --depth 1 "$seed" "$revision"
    else
        git -C "$source_dir" fetch --depth 1 https://github.com/kunkundi/minirtc.git "$revision"
    fi
    git -C "$source_dir" checkout --detach FETCH_HEAD
fi
if [[ $(git -C "$source_dir" rev-parse HEAD) != "$revision" ]]; then
    printf 'MiniRTC source directory must be at %s: %s\n' "$revision" "$source_dir" >&2
    exit 1
fi
git -C "$source_dir" checkout -- .
for patch in "${patches[@]}"; do
    git -C "$source_dir" apply --ignore-whitespace "$patch"
done

recipe="$source_dir/xmake.lua"
if ! grep -Fq 'P2PREMOTE_LINUX_MINIRTC_TARGET' "$recipe"; then
    cat >> "$recipe" <<'LUA'

target_end()
includes(os.getenv("P2PREMOTE_LINUX_MINIRTC_TARGET"))
LUA
fi

export P2PREMOTE_LINUX_MINIRTC_TARGET="$tools_dir/linux-target.lua"
export P2PREMOTE_LINUX_MINIRTC_OUTPUT="$source_dir/build/p2premote"
library="$P2PREMOTE_LINUX_MINIRTC_OUTPUT/libp2premote_minirtc.so"
export_list="$P2PREMOTE_LINUX_MINIRTC_OUTPUT/exports.sha256"
fingerprint=$(sha256sum "$tools_dir/linux-exports.map" "$tools_dir/linux_h264.h" "$tools_dir/linux_h264.cpp" "$P2PREMOTE_LINUX_MINIRTC_TARGET" "${patches[@]}")
if [[ ! -f "$export_list" ]] || [[ $(<"$export_list") != "$fingerprint" ]]; then
    rm -f -- "$library"
fi
cd -- "$source_dir"
"$xmake" f -p linux -m release --USE_CUDA=false -y
"$xmake" b -j "${P2PREMOTE_BUILD_JOBS:-4}" -y p2premote_minirtc
install -m 0644 src/api/minirtc.h "$prefix/include/minirtc.h"
install -m 0755 "$library" "$prefix/lib/libp2premote_minirtc.so"
printf '%s\n' "$fingerprint" > "$export_list"
printf 'MiniRTC installed to %s\n' "$prefix"
