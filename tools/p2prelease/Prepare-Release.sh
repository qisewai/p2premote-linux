#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "Usage: $0 <p2prelease> <stages-directory> <previous-assets-directory> <output-directory>" >&2
    exit 2
fi
packager=$1
stages=$2
previous=$3
output=$4
version=$(tr -d '\r\n' < VERSION)
pubkey=cmake/update-public.key
: "${GITHUB_REPOSITORY:?Set the GitHub owner/repository for the release URLs}"
: "${P2PREMOTE_UPDATE_PRIVATE_KEY:?The release signing key is required}"

mkdir -p "$output"
for platform in windows-x64 linux-x64; do
    stage="$stages/$platform"
    manifest="$output/update-$platform.json"
    archive="$output/p2premote-$version-$platform.tar.gz"
    if [[ $platform == windows-x64 ]]; then
        test -f "$stage/p2premote_qt.exe"
        test -f "$stage/p2premote_service.exe"
    else
        test -x "$stage/usr/local/libexec/p2premote/gui/p2premote"
        test -x "$stage/usr/local/libexec/p2premote/p2premote_linux_host"
        pub_b64=$(tr -d '\r\n' < "$pubkey")
        [[ $GITHUB_REPOSITORY =~ ^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$ ]]
        [[ $pub_b64 =~ ^[A-Za-z0-9+/=]{44}$ ]]
        sed -e "s|^DEFAULT_REPO=.*|DEFAULT_REPO='$GITHUB_REPOSITORY'|" \
            -e "s|^DEFAULT_PUBLIC_KEY=.*|DEFAULT_PUBLIC_KEY='$pub_b64'|" \
            tools/p2prelease/install.sh > "$stage/install.sh"
        chmod 0755 "$stage/install.sh"
        bash -n "$stage/install.sh"
        printf '%s\n' "$version" > "$stage/VERSION"
        chmod 0644 "$stage/VERSION"
        install -m 0755 -- "$stage/install.sh" "$output/install.sh"
    fi

    "$packager" pack --dir "$stage" --target "$platform" --version "$version" \
        --github "$GITHUB_REPOSITORY" --out "$output" --changelog CHANGELOG.md
    "$packager" verify --manifest "$manifest" --archive "$archive" --pubkey "$pubkey"
    if [[ $platform == linux-x64 ]]; then
        tar -tzf "$archive" install.sh >/dev/null
        tar -tzf "$archive" VERSION >/dev/null
    fi

    old_manifest="$previous/update-$platform.json"
    [[ -f $old_manifest ]] || continue
    old_version=$(jq -er '.version | select(test("^[0-9]+\\.[0-9]+\\.[0-9]+$"))' "$old_manifest") || continue
    old_archive="$previous/p2premote-$old_version-$platform.tar.gz"
    [[ -f $old_archive ]] || continue
    if ! "$packager" verify --manifest "$old_manifest" --archive "$old_archive" --pubkey "$pubkey"; then
        echo "Previous $platform archive is unavailable for delta generation; publishing the full archive."
        continue
    fi

    # The standard bsdiff generator holds the source and its suffix array in memory.
    size=$(stat -c %s "$archive")
    limit=$((192 * 1024 * 1024))
    if (( size > limit || $(stat -c %s "$old_archive") > limit )); then
        echo "Skipping $platform delta for large archives."
        continue
    fi
    patch="$output/p2premote-$old_version-to-$version-$platform.delta"
    if ! bsdiff "$old_archive" "$archive" "$patch" || (( $(stat -c %s "$patch") >= size )); then
        rm -f -- "$patch"
        echo "No smaller $platform delta was produced."
        continue
    fi
    "$packager" add-delta --manifest "$manifest" --archive "$archive" \
        --from-manifest "$old_manifest" --from-archive "$old_archive" --patch "$patch" --pubkey "$pubkey"
    "$packager" verify --manifest "$manifest" --archive "$archive" --pubkey "$pubkey"
done
