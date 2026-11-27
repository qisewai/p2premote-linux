#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage: Package-LinuxTarball.sh <configured Linux x86_64 build directory> <output directory> [--key <private_key_file>] [--repo <owner/repo>]
EOF
}

if (( $# < 2 )); then
    usage
    exit 2
fi

if [[ $(uname -m) != x86_64 ]]; then
    echo "Build the Linux x86_64 tarball on an x86_64 Linux system." >&2
    exit 1
fi

build_dir=$(realpath -- "$1")
output_dir=$(realpath -m -- "$2")
shift 2

key_file=""
github_repo="qisewai/p2premote-linux"
while (( $# > 0 )); do
    case "$1" in
        --key)
            (( $# >= 2 )) || { echo "Missing value for --key" >&2; exit 2; }
            key_file=$2
            shift 2
            ;;
        --repo)
            (( $# >= 2 )) || { echo "Missing value for --repo" >&2; exit 2; }
            github_repo=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 2
            ;;
    esac
done

[[ $github_repo =~ ^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$ ]] || {
    echo "Invalid --repo value: $github_repo" >&2
    exit 2
}

script_dir=$(dirname -- "$(realpath -- "$0")")
repo_root=$(realpath -- "$script_dir/../..")
version=$(tr -d '\r\n' < "$repo_root/VERSION")
pubkey_file="$repo_root/cmake/update-public.key"
pub_b64=$(tr -d '\r\n' < "$pubkey_file")
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Invalid VERSION: $version" >&2; exit 1; }
[[ $pub_b64 =~ ^[A-Za-z0-9+/=]{44}$ ]] || { echo "Invalid public key in $pubkey_file" >&2; exit 1; }

for cmd in cmake patchelf python3 openssl tar sha256sum; do
    command -v "$cmd" >/dev/null || { echo "Required command not found: $cmd" >&2; exit 1; }
done

work=$(mktemp -d)
stage="$work/stage"
mkdir -p -- "$stage" "$output_dir"
build_archive=""
cleanup() {
    rm -rf -- "$work"
    if [[ -n $build_archive ]]; then
        rm -f -- "$build_archive"
    fi
}
trap cleanup EXIT

DESTDIR="$stage" cmake --install "$build_dir"
runtime="$stage/usr/local/libexec/p2premote"
if [[ ! -x "$runtime/gui/p2premote" || ! -x "$runtime/p2premote_linux_host" || ! -x "$stage/usr/local/bin/p2premote" ]]; then
    echo "Configure and build the complete linux-x64 preset with CMAKE_INSTALL_PREFIX=/usr/local." >&2
    exit 1
fi

python3 "$script_dir/Bundle-LinuxRuntime.py" "$runtime"

sed -e "s|^DEFAULT_REPO=.*|DEFAULT_REPO='$github_repo'|" \
    -e "s|^DEFAULT_PUBLIC_KEY=.*|DEFAULT_PUBLIC_KEY='$pub_b64'|" \
    "$script_dir/install.sh" > "$stage/install.sh"
chmod 0755 -- "$stage/install.sh"
bash -n "$stage/install.sh"
printf '%s\n' "$version" > "$stage/VERSION"
chmod 0644 -- "$stage/VERSION"
install -m 0755 -- "$stage/install.sh" "$output_dir/install.sh"

archive_name="p2premote-$version-linux-x64.tar.gz"
packager="$build_dir/p2prelease"
if [[ ( -n $key_file || -n ${P2PREMOTE_UPDATE_PRIVATE_KEY:-} ) && -x $packager ]]; then
    pack_args=(
        pack --dir "$stage" --target linux-x64 --version "$version"
        --github "$github_repo" --out "$output_dir"
    )
    if [[ -f "$repo_root/CHANGELOG.md" ]]; then
        pack_args+=(--changelog "$repo_root/CHANGELOG.md")
    fi
    if [[ -n $key_file ]]; then
        pack_args+=(--key "$key_file")
    fi
    "$packager" "${pack_args[@]}"
    "$packager" verify \
        --manifest "$output_dir/update-linux-x64.json" \
        --archive "$output_dir/$archive_name" \
        --pubkey "$pubkey_file"
else
    build_archive=$(mktemp --tmpdir="$output_dir" .p2premote-tarball.XXXXXXXX.tar.gz)
    tar -czf "$build_archive" --sort=name --owner=0 --group=0 --numeric-owner -C "$stage" .
    chmod 0644 -- "$build_archive"
    mv -fT -- "$build_archive" "$output_dir/$archive_name"
    build_archive=""
    (cd "$output_dir" && sha256sum "$archive_name" > "$archive_name.sha256")
    echo "Archive: $output_dir/$archive_name (unsigned local build)"
fi

tar -tzf "$output_dir/$archive_name" install.sh >/dev/null
tar -tzf "$output_dir/$archive_name" VERSION >/dev/null
