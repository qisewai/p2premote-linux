#!/bin/bash
set -euo pipefail

root=$(dirname -- "$(readlink -f -- "$0")")
bundle="$root/usr/local/libexec/p2premote"
runtime=/usr/local/libexec/p2premote
launcher=/usr/local/bin/p2premote

is_installed() {
    [[ -x "$launcher" && -x "$runtime/gui/p2premote" && -x "$runtime/p2premote_linux_host" ]] &&
        cmp -s "$bundle/.appimage-build" "$runtime/.appimage-build"
}

install_runtime() {
    if [[ $EUID != 0 ]]; then
        echo "Administrator authorization is required to install P2P Remote." >&2
        return 1
    fi
    export PATH=/usr/sbin:/usr/bin:/sbin:/bin
    umask 022

    if [[ $(stat -c '%u %a' -- "$(dirname -- "$root")") != '0 700' ]]; then
        echo "Use a private, root-owned temporary directory to extract the AppImage before installing." >&2
        return 1
    fi

    local directory owner mode
    for directory in /usr/local /usr/local/libexec /usr/local/bin /usr/local/share \
        /usr/local/share/applications /etc/xdg /etc/xdg/autostart; do
        [[ ! -L "$directory" ]] || return 1
        [[ -d "$directory" ]] || install -d -m 0755 -- "$directory"
        read -r owner mode < <(stat -c '%u %a' -- "$directory")
        if [[ $owner != 0 ]] || (( (8#$mode & 0022) != 0 )); then
            echo "Unsafe P2P Remote installation directory: $directory" >&2
            return 1
        fi
    done

    local lock=/run/lock/p2premote-host.lock
    (umask 077; set -o noclobber; : > "$lock") 2>/dev/null || :
    if [[ -L "$lock" || ! -f "$lock" || $(stat -c '%u %a' -- "$lock") != '0 600' ]]; then
        echo "Could not acquire the P2P Remote service lock." >&2
        return 1
    fi
    exec 9<"$lock"
    flock --exclusive 9
    if is_installed; then
        exec 9<&-
        return 0
    fi

    local targets=("$runtime" "$launcher" /usr/local/share/p2premote
        /usr/local/share/applications/p2premote.desktop /etc/xdg/autostart/p2premote-clipboard.desktop)
    local stages=() active_units=() replaced=() index target stage unit
    local committed=0
    rollback_install() {
        local result=$?
        trap - EXIT
        if (( ! committed )); then
            set +e
            if (( ${#active_units[@]} )); then systemctl stop "${active_units[@]}"; fi
            for ((index=${#stages[@]}-1; index>=0; --index)); do
                stage=${stages[index]}
                target=${targets[index]}
                if [[ ${replaced[index]:-0} == 1 ]]; then
                    mv -T -- "$target" "$stage/rejected"
                fi
                if [[ -e "$stage/previous" || -L "$stage/previous" ]]; then
                    if ! mv -T -- "$stage/previous" "$target"; then
                        echo "Could not restore $target. Previous files are at $stage/previous." >&2
                        stages[index]=''
                    fi
                fi
            done
            if (( ${#active_units[@]} )); then systemctl start "${active_units[@]}"; fi
        fi
        for stage in "${stages[@]}"; do
            if [[ -n $stage ]]; then rm -rf -- "$stage"; fi
        done
        exit "$result"
    }
    trap rollback_install EXIT
    trap 'exit 1' HUP INT TERM

    for target in "${targets[@]}"; do
        [[ -e "$root$target" ]] || { echo "Missing AppImage payload: $target" >&2; exit 1; }
        stage=$(mktemp -d -- "$(dirname -- "$target")/.p2premote-install.XXXXXXXX")
        stages+=("$stage")
        cp -a --no-preserve=ownership -- "$root$target" "$stage/new"
        chmod -R a+rX,go-w -- "$stage/new"
    done
    for unit in p2premote-host.service p2premote-cloud-host.service; do
        if systemctl is-active --quiet "$unit"; then
            active_units+=("$unit")
            systemctl stop "$unit"
        fi
    done
    for index in "${!targets[@]}"; do
        stage=${stages[index]}
        target=${targets[index]}
        if [[ -e "$target" || -L "$target" ]]; then mv -T -- "$target" "$stage/previous"; fi
        mv -T -- "$stage/new" "$target"
        replaced[index]=1
    done
    if (( ${#active_units[@]} )); then systemctl start "${active_units[@]}"; fi
    committed=1
    for stage in "${stages[@]}"; do rm -rf -- "$stage"; done
    trap - EXIT HUP INT TERM
    exec 9<&-
}

case ${1:-} in
    --install|--install-and-enable-host)
        if (( $# != 1 )); then exit 2; fi
        install_runtime
        if [[ $1 == --install-and-enable-host ]]; then
            exec "$runtime/p2premote_linux_host" --install-host-service
        fi
        ;;
    *)
        if is_installed; then
            unset APPDIR APPIMAGE
            exec "$launcher" "$@"
        fi
        exec "$bundle/gui/p2premote" "$@"
        ;;
esac
