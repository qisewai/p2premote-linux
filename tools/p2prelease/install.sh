#!/usr/bin/env bash
set -euo pipefail

DEFAULT_REPO='qisewai/p2premote-linux'
DEFAULT_PUBLIC_KEY='yNb18EQ5GCXSGd+UcCTS5WBJePiJs29IVn68u5UOrYU='

TEMP_WORK=""
RB_TARGETS=()
RB_STAGES=()
RB_ACTIVE_UNITS=()
RB_REPLACED=()
RB_REFRESH_UNIT=0
RB_COMMITTED=0

cleanup_temp_work() {
    if [[ -n ${TEMP_WORK:-} ]]; then
        rm -rf -- "$TEMP_WORK"
        TEMP_WORK=""
    fi
}

rollback_install() {
    local result=$?
    local index stage target unit
    trap - EXIT HUP INT TERM
    if (( ! RB_COMMITTED )); then
        set +e
        if (( ${#RB_ACTIVE_UNITS[@]} )) && command -v systemctl >/dev/null 2>&1; then
            systemctl stop "${RB_ACTIVE_UNITS[@]}" </dev/null
        fi
        for ((index=${#RB_STAGES[@]}-1; index>=0; --index)); do
            stage=${RB_STAGES[index]}
            target=${RB_TARGETS[index]}
            if [[ ${RB_REPLACED[index]:-0} == 1 ]]; then
                mv -T -- "$target" "$stage/rejected"
            fi
            if [[ -e "$stage/previous" || -L "$stage/previous" ]]; then
                if ! mv -T -- "$stage/previous" "$target"; then
                    echo "无法恢复 $target，旧版本文件保留在 $stage/previous。" >&2
                    RB_STAGES[index]=''
                fi
            fi
        done
        if (( RB_REFRESH_UNIT )) && command -v systemctl >/dev/null 2>&1; then
            systemctl daemon-reload </dev/null
        fi
        if (( ${#RB_ACTIVE_UNITS[@]} )) && command -v systemctl >/dev/null 2>&1; then
            for unit in "${RB_ACTIVE_UNITS[@]}"; do
                if should_restart_unit "$unit"; then
                    systemctl start "$unit" </dev/null
                fi
            done
        fi
    fi
    for stage in "${RB_STAGES[@]}"; do
        if [[ -n $stage ]]; then rm -rf -- "$stage"; fi
    done
    RB_TARGETS=()
    RB_STAGES=()
    RB_ACTIVE_UNITS=()
    RB_REPLACED=()
    RB_REFRESH_UNIT=0
    cleanup_temp_work
    exit "$result"
}

usage() {
    cat <<'EOF'
P2P Remote Linux 安装脚本

用法:
  sudo bash install.sh [选项]
  curl -fsSL https://github.com/qisewai/p2premote-linux/releases/latest/download/install.sh | sudo bash
  curl -fsSL https://github.com/qisewai/p2premote-linux/releases/latest/download/install.sh | sudo bash -s -- [选项]

选项:
  --archive <path>       从本地 .tar.gz 安装包离线安装
  --version <X.Y.Z>      在线安装指定版本
  --repo <owner/repo>    指定 GitHub 仓库 (默认: qisewai/p2premote-linux)
  --mirror <https://...> 通过 HTTPS 镜像前缀加速下载 (仍强制 Ed25519 验签)
  --manifest-url <url>   指定 update-linux-x64.json 清单地址
  --pubkey <b64_or_file> 指定 Ed25519 公钥 (Base64 或公钥文件路径)
  --uninstall            卸载系统安装和被控服务 (保留 ~/.config/P2PRemote)
  -h, --help             显示此帮助信息
EOF
}

require_cmd() {
    local cmd
    for cmd in "$@"; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            echo "缺少必需命令: $cmd" >&2
            return 1
        fi
    done
}

download_file() {
    local url=$1
    local dest=$2
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 --connect-timeout 15 --max-time 600 -o "$dest" "$url" </dev/null
    elif command -v wget >/dev/null 2>&1; then
        wget -q -T 30 -O "$dest" "$url" </dev/null
    else
        echo "需要 curl 或 wget 以下载安装包。" >&2
        return 1
    fi
}

apply_mirror() {
    local url=$1
    local mirror=$2
    if [[ -n $mirror && $url == https://github.com/* ]]; then
        printf '%s/%s\n' "$mirror" "$url"
    else
        printf '%s\n' "$url"
    fi
}

parse_manifest() {
    local file=$1
    local parsed
    if command -v jq >/dev/null 2>&1; then
        parsed=$(jq -r '
            def s(v): if v == null then "" else (v | tostring) end;
            [s(.version), s(.url), s(.size), s(.signature), s(.target)] | .[]
        ' "$file" </dev/null)
    elif command -v python3 >/dev/null 2>&1; then
        parsed=$(python3 -c '
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)
if not isinstance(data, dict):
    sys.exit(1)
for key in ("version", "url", "size", "signature", "target"):
    val = data.get(key, "")
    s = "" if val is None else str(val)
    if "\n" in s or "\r" in s:
        sys.exit(1)
    print(s)
' "$file" </dev/null)
    else
        parsed=$(awk '
            /^  "version":[[:space:]]*"/ {
                if (v == "") { v = $0; sub(/^  "version":[[:space:]]*"/, "", v); sub(/",?[[:space:]]*$/, "", v) }
            }
            /^  "url":[[:space:]]*"/ {
                if (u == "") { u = $0; sub(/^  "url":[[:space:]]*"/, "", u); sub(/",?[[:space:]]*$/, "", u) }
            }
            /^  "size":[[:space:]]*[0-9]+/ {
                if (sz == "") { sz = $0; sub(/^  "size":[[:space:]]*/, "", sz); sub(/,?[[:space:]]*$/, "", sz) }
            }
            /^  "signature":[[:space:]]*"/ {
                if (sig == "") { sig = $0; sub(/^  "signature":[[:space:]]*"/, "", sig); sub(/",?[[:space:]]*$/, "", sig) }
            }
            /^  "target":[[:space:]]*"/ {
                if (t == "") { t = $0; sub(/^  "target":[[:space:]]*"/, "", t); sub(/",?[[:space:]]*$/, "", t) }
            }
            END {
                printf "%s\n%s\n%s\n%s\n%s\n", v, u, sz, sig, t
            }
        ' "$file" </dev/null)
    fi

    local lines=()
    local line
    while IFS= read -r line; do
        lines+=("$line")
    done <<< "$parsed"
    if (( ${#lines[@]} < 4 )); then
        echo "更新清单格式无效或字段缺失。" >&2
        return 1
    fi
    MANIFEST_VERSION=${lines[0]:-}
    MANIFEST_URL=${lines[1]:-}
    MANIFEST_SIZE=${lines[2]:-}
    MANIFEST_SIGNATURE=${lines[3]:-}
    MANIFEST_TARGET=${lines[4]:-}
}

verify_archive_signature() {
    local archive=$1
    local pub_b64=$2
    local sig_b64=$3
    local work_dir=$4

    pub_b64=$(printf '%s' "$pub_b64" | tr -d '\r\n[:space:]')
    sig_b64=$(printf '%s' "$sig_b64" | tr -d '\r\n[:space:]')
    if [[ ! $pub_b64 =~ ^[A-Za-z0-9+/=]+$ ]] || [[ ! $sig_b64 =~ ^[A-Za-z0-9+/=]+$ ]]; then
        echo "公钥或签名的 Base64 格式无效。" >&2
        return 1
    fi

    printf '\x30\x2a\x30\x05\x06\x03\x2b\x65\x70\x03\x21\x00' > "$work_dir/pub.der"
    if ! printf '%s' "$pub_b64" | base64 -d >> "$work_dir/pub.der" 2>/dev/null; then
        echo "无法解码 Ed25519 公钥。" >&2
        return 1
    fi
    if [[ $(stat -c %s -- "$work_dir/pub.der") -ne 44 ]]; then
        echo "Ed25519 公钥长度无效。" >&2
        return 1
    fi
    if ! openssl pkey -pubin -inform DER -in "$work_dir/pub.der" -out "$work_dir/pub.pem" </dev/null 2>/dev/null; then
        echo "无法加载 Ed25519 公钥（需要 OpenSSL 3.0+）。" >&2
        return 1
    fi

    if ! printf '%s' "$sig_b64" | base64 -d > "$work_dir/sig.bin" 2>/dev/null; then
        echo "无法解码安装包签名。" >&2
        return 1
    fi
    if [[ $(stat -c %s -- "$work_dir/sig.bin") -ne 64 ]]; then
        echo "安装包签名长度无效。" >&2
        return 1
    fi

    openssl dgst -sha256 -binary "$archive" > "$work_dir/digest.bin" </dev/null
    if ! openssl pkeyutl -verify -pubin -inkey "$work_dir/pub.pem" -rawin \
            -in "$work_dir/digest.bin" -sigfile "$work_dir/sig.bin" >/dev/null 2>&1 </dev/null; then
        echo "安装包 Ed25519 签名校验失败，已中止安装。" >&2
        return 1
    fi
}

validate_dir_symlinks() {
    local subtree_root=$1
    local subtree_real link target link_dir_real resolved bad_special
    subtree_real=$(readlink -f -- "$subtree_root")

    bad_special=$(find "$subtree_root" \( -type b -o -type c -o -type p -o -type s \) -print -quit)
    if [[ -n $bad_special ]]; then
        echo "安装包包含不允许的特殊设备文件: $bad_special" >&2
        return 1
    fi

    while IFS= read -r -d '' link; do
        target=$(readlink -- "$link")
        if [[ -z $target || $target == /* ]]; then
            echo "拒绝绝对路径符号链接: $link -> $target" >&2
            return 1
        fi
        link_dir_real=$(readlink -f -- "$(dirname -- "$link")")
        if [[ $link_dir_real != "$subtree_real" && $link_dir_real != "$subtree_real"/* ]]; then
            echo "符号链接所在目录越界: $link" >&2
            return 1
        fi
        resolved=$(readlink -m -- "$link_dir_real/$target")
        if [[ $resolved != "$subtree_real" && $resolved != "$subtree_real"/* ]]; then
            echo "拒绝越界符号链接: $link -> $target" >&2
            return 1
        fi
    done < <(find "$subtree_root" -type l -print0)
}

validate_payload() {
    local payload_root=$1
    local required_dirs=(
        "$payload_root/usr/local/libexec/p2premote"
        "$payload_root/usr/local/share/p2premote"
    )
    local required_files=(
        "$payload_root/usr/local/libexec/p2premote/p2premote_linux_host"
        "$payload_root/usr/local/libexec/p2premote/gui/p2premote"
        "$payload_root/usr/local/libexec/p2premote/p2premote_clipboard"
        "$payload_root/usr/local/libexec/p2premote/units/p2premote-host.service"
        "$payload_root/usr/local/bin/p2premote"
        "$payload_root/usr/local/share/applications/p2premote.desktop"
        "$payload_root/etc/xdg/autostart/p2premote-clipboard.desktop"
    )
    local p
    for p in "${required_dirs[@]}"; do
        if [[ -L $p || ! -d $p ]]; then
            echo "安装包目录结构不完整或包含非法符号链接: $p" >&2
            return 1
        fi
    done
    for p in "${required_files[@]}"; do
        if [[ -L $p || ! -f $p ]]; then
            echo "安装包缺少必需文件: $p" >&2
            return 1
        fi
    done
    for p in \
        "$payload_root/usr/local/libexec/p2premote/p2premote_linux_host" \
        "$payload_root/usr/local/libexec/p2premote/gui/p2premote" \
        "$payload_root/usr/local/libexec/p2premote/p2premote_clipboard" \
        "$payload_root/usr/local/bin/p2premote"; do
        if [[ ! -x $p ]]; then
            echo "安装包文件缺少可执行权限: $p" >&2
            return 1
        fi
    done

    validate_dir_symlinks "$payload_root/usr/local/libexec/p2premote"
    validate_dir_symlinks "$payload_root/usr/local/share/p2premote"
}

check_safe_dir() {
    local directory=$1
    local create_if_missing=${2:-1}
    local owner mode
    if [[ -L $directory ]]; then
        echo "不安全的符号链接安装目录: $directory" >&2
        return 1
    fi
    if [[ ! -d $directory ]]; then
        if [[ $create_if_missing != 1 ]]; then
            echo "安装目录不存在: $directory" >&2
            return 1
        fi
        install -d -m 0755 -o 0 -g 0 -- "$directory"
    fi
    read -r owner mode < <(stat -c '%u %a' -- "$directory")
    if [[ $owner != 0 ]] || (( (8#$mode & 0022) != 0 )); then
        echo "不安全的安装目录权限: $directory (uid=$owner mode=$mode)" >&2
        return 1
    fi
}

acquire_lock() {
    local lock=/run/lock/p2premote-host.lock
    [[ ! -L /run ]] || { echo "不安全的 /run 目录。" >&2; return 1; }
    [[ -d /run ]] || install -d -m 0755 -o 0 -g 0 -- /run
    [[ ! -L /run/lock ]] || { echo "不安全的 /run/lock 目录。" >&2; return 1; }
    [[ -d /run/lock ]] || install -d -m 0755 -o 0 -g 0 -- /run/lock
    (umask 077; set -o noclobber; : > "$lock") 2>/dev/null || :
    if [[ -L "$lock" || ! -f "$lock" || $(stat -c '%u %a' -- "$lock") != '0 600' ]]; then
        echo "无法获取 P2P Remote 服务锁: $lock" >&2
        return 1
    fi
    exec 9<"$lock"
    flock --exclusive 9
}

should_restart_unit() {
    local unit=$1
    if [[ $unit == p2premote-host.service ]]; then
        return 0
    fi
    [[ -f "/etc/systemd/system/$unit" || -f "/lib/systemd/system/$unit" || -f "/usr/lib/systemd/system/$unit" ]]
}

install_from_root() {
    local payload_root=$1
    local version_label=${2:-}
    local had_existing=0
    if [[ -e /usr/local/libexec/p2premote || -e /etc/systemd/system/p2premote-host.service || -d /etc/p2premote ]]; then
        had_existing=1
    fi

    validate_payload "$payload_root"
    if [[ -z $version_label && -f "$payload_root/VERSION" ]]; then
        version_label=$(tr -d '\r\n' < "$payload_root/VERSION")
    fi

    local directory
    for directory in /usr/local /usr/local/libexec /usr/local/bin /usr/local/share \
        /usr/local/share/applications /etc/xdg /etc/xdg/autostart; do
        check_safe_dir "$directory" 1
    done

    acquire_lock

    RB_TARGETS=(
        /usr/local/libexec/p2premote
        /usr/local/bin/p2premote
        /usr/local/share/p2premote
        /usr/local/share/applications/p2premote.desktop
        /etc/xdg/autostart/p2premote-clipboard.desktop
    )
    local sources=(
        "$payload_root/usr/local/libexec/p2premote"
        "$payload_root/usr/local/bin/p2premote"
        "$payload_root/usr/local/share/p2premote"
        "$payload_root/usr/local/share/applications/p2premote.desktop"
        "$payload_root/etc/xdg/autostart/p2premote-clipboard.desktop"
    )
    RB_REFRESH_UNIT=0
    if [[ -e /etc/systemd/system/p2premote-host.service || -L /etc/systemd/system/p2premote-host.service ]]; then
        if [[ -L /etc/systemd/system/p2premote-host.service || ! -f /etc/systemd/system/p2premote-host.service ]]; then
            echo "不安全的 systemd 单元路径: /etc/systemd/system/p2premote-host.service" >&2
            exec 9<&-
            return 1
        fi
        check_safe_dir /etc/systemd 0
        check_safe_dir /etc/systemd/system 0
        RB_TARGETS+=(/etc/systemd/system/p2premote-host.service)
        sources+=("$payload_root/usr/local/libexec/p2premote/units/p2premote-host.service")
        RB_REFRESH_UNIT=1
    fi

    RB_STAGES=()
    RB_ACTIVE_UNITS=()
    RB_REPLACED=()
    RB_COMMITTED=0
    local index target source stage unit
    trap rollback_install EXIT
    trap 'exit 1' HUP INT TERM

    for index in "${!RB_TARGETS[@]}"; do
        target=${RB_TARGETS[index]}
        source=${sources[index]}
        stage=$(mktemp -d -- "$(dirname -- "$target")/.p2premote-install.XXXXXXXX")
        RB_STAGES+=("$stage")
        cp -a --no-preserve=ownership -- "$source" "$stage/new"
        chown -hR 0:0 -- "$stage/new"
        chmod -R u-s,g-s,go-w,a+rX -- "$stage/new"
        if [[ $target == *.desktop || $target == *.service ]]; then
            chmod 0644 -- "$stage/new"
        fi
    done

    if command -v systemctl >/dev/null 2>&1; then
        for unit in p2premote-host.service p2premote-cloud-host.service; do
            if systemctl is-active --quiet "$unit" </dev/null 2>/dev/null; then
                RB_ACTIVE_UNITS+=("$unit")
                systemctl stop "$unit" </dev/null
            fi
        done
    fi

    for index in "${!RB_TARGETS[@]}"; do
        stage=${RB_STAGES[index]}
        target=${RB_TARGETS[index]}
        if [[ -e "$target" || -L "$target" ]]; then
            mv -T -- "$target" "$stage/previous"
        fi
        mv -T -- "$stage/new" "$target"
        RB_REPLACED[index]=1
    done

    if (( RB_REFRESH_UNIT )) && command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload </dev/null
    fi
    if (( ${#RB_ACTIVE_UNITS[@]} )) && command -v systemctl >/dev/null 2>&1; then
        for unit in "${RB_ACTIVE_UNITS[@]}"; do
            if should_restart_unit "$unit"; then
                systemctl start "$unit" </dev/null
                systemctl is-active --quiet "$unit" </dev/null
            fi
        done
    fi

    RB_COMMITTED=1
    for stage in "${RB_STAGES[@]}"; do
        rm -rf -- "$stage"
    done
    RB_TARGETS=()
    RB_STAGES=()
    RB_ACTIVE_UNITS=()
    RB_REPLACED=()
    RB_REFRESH_UNIT=0
    trap cleanup_temp_work EXIT
    trap 'exit 1' HUP INT TERM
    exec 9<&-

    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database /usr/local/share/applications >/dev/null 2>&1 </dev/null || true
    fi

    if [[ -n $version_label ]]; then
        echo "已将 P2P Remote ($version_label) 安装到 /usr/local。"
    else
        echo "已将 P2P Remote 安装到 /usr/local。"
    fi
    echo "启动客户端: p2premote (或在应用菜单打开 P2P Remote)"
    if (( had_existing )); then
        echo "已保留 /etc/p2premote 下的现有被控配置。"
    else
        echo "首次安装默认关闭被控接收，可在「本机 → 允许远程控制本机」启用。"
    fi
}

extract_archive_to_temp() {
    local archive_path=$1
    local dest_dir=$2
    mkdir -p -m 0755 -- "$dest_dir"
    (umask 022 && tar --no-same-owner -xzf "$archive_path" -C "$dest_dir" </dev/null)
}

install_online() {
    local repo=$1
    local req_version=$2
    local mirror=$3
    local manifest_url=$4
    local pubkey_b64=$5

    require_cmd openssl tar base64

    local allow_local_http=0
    if [[ -z $manifest_url ]]; then
        if [[ -n $req_version ]]; then
            manifest_url="https://github.com/$repo/releases/download/v$req_version/update-linux-x64.json"
        else
            manifest_url="https://github.com/$repo/releases/latest/download/update-linux-x64.json"
        fi
    elif [[ $manifest_url =~ ^http://(127\.0\.0\.1|localhost)(:[0-9]+)?/ ]]; then
        allow_local_http=1
    fi

    TEMP_WORK=$(mktemp -d)
    chmod 0700 -- "$TEMP_WORK"

    local fetch_manifest_url
    fetch_manifest_url=$(apply_mirror "$manifest_url" "$mirror")
    local manifest_file="$TEMP_WORK/update-linux-x64.json"
    echo "正在获取发布清单: $fetch_manifest_url"
    download_file "$fetch_manifest_url" "$manifest_file"

    parse_manifest "$manifest_file"

    if [[ ! $MANIFEST_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "清单版本号无效: $MANIFEST_VERSION" >&2
        return 1
    fi
    if [[ -n $req_version && $MANIFEST_VERSION != "$req_version" ]]; then
        echo "清单版本 ($MANIFEST_VERSION) 与请求版本 ($req_version) 不一致。" >&2
        return 1
    fi
    if [[ -n $MANIFEST_TARGET && $MANIFEST_TARGET != linux-x64 ]]; then
        echo "清单目标平台不匹配: $MANIFEST_TARGET" >&2
        return 1
    fi
    if [[ ! $MANIFEST_SIZE =~ ^[1-9][0-9]*$ ]]; then
        echo "清单文件大小无效: $MANIFEST_SIZE" >&2
        return 1
    fi
    if [[ -z $MANIFEST_SIGNATURE ]]; then
        echo "清单缺少签名。" >&2
        return 1
    fi
    if (( allow_local_http )); then
        if [[ ! $MANIFEST_URL =~ ^(https://|http://(127\.0\.0\.1|localhost)(:[0-9]+)?/) ]]; then
            echo "不安全的安装包下载地址: $MANIFEST_URL" >&2
            return 1
        fi
    elif [[ $MANIFEST_URL != https://* ]]; then
        echo "安装包下载地址必须使用 HTTPS: $MANIFEST_URL" >&2
        return 1
    fi

    local fetch_archive_url
    fetch_archive_url=$(apply_mirror "$MANIFEST_URL" "$mirror")
    local archive_file="$TEMP_WORK/p2premote-$MANIFEST_VERSION-linux-x64.tar.gz"
    echo "正在下载安装包 P2P Remote $MANIFEST_VERSION..."
    download_file "$fetch_archive_url" "$archive_file"

    local actual_size
    actual_size=$(stat -c %s -- "$archive_file")
    if [[ $actual_size != "$MANIFEST_SIZE" ]]; then
        echo "安装包大小不匹配: 预期 $MANIFEST_SIZE 字节，实际 $actual_size 字节。" >&2
        return 1
    fi

    verify_archive_signature "$archive_file" "$pubkey_b64" "$MANIFEST_SIGNATURE" "$TEMP_WORK"

    local unpacked="$TEMP_WORK/unpacked"
    extract_archive_to_temp "$archive_file" "$unpacked"
    if [[ -f "$unpacked/VERSION" ]]; then
        local pkg_version
        pkg_version=$(tr -d '\r\n' < "$unpacked/VERSION")
        if [[ $pkg_version != "$MANIFEST_VERSION" ]]; then
            echo "压缩包内 VERSION ($pkg_version) 与清单版本 ($MANIFEST_VERSION) 不一致。" >&2
            return 1
        fi
    fi

    install_from_root "$unpacked" "$MANIFEST_VERSION"
}

uninstall_system() {
    local has_host_state=0
    if [[ -e /etc/systemd/system/p2premote-host.service || -L /etc/systemd/system/p2premote-host.service || \
          -e /etc/systemd/system/p2premote-cloud-host.service || -L /etc/systemd/system/p2premote-cloud-host.service || \
          -d /etc/p2premote ]]; then
        has_host_state=1
    elif command -v systemctl >/dev/null 2>&1; then
        if systemctl is-active --quiet p2premote-host.service </dev/null 2>/dev/null || \
           systemctl is-enabled --quiet p2premote-host.service </dev/null 2>/dev/null || \
           systemctl is-active --quiet p2premote-cloud-host.service </dev/null 2>/dev/null || \
           systemctl is-enabled --quiet p2premote-cloud-host.service </dev/null 2>/dev/null; then
            has_host_state=1
        fi
    fi

    # Invoke p2premote_linux_host BEFORE acquiring the lock to avoid self-deadlock.
    if (( has_host_state )) && [[ -x /usr/local/libexec/p2premote/p2premote_linux_host ]] && command -v systemctl >/dev/null 2>&1; then
        /usr/local/libexec/p2premote/p2premote_linux_host --uninstall-host-service </dev/null
    fi

    acquire_lock

    local unit
    if command -v systemctl >/dev/null 2>&1; then
        for unit in p2premote-host.service p2premote-cloud-host.service; do
            if systemctl is-active --quiet "$unit" </dev/null 2>/dev/null || \
               systemctl is-enabled --quiet "$unit" </dev/null 2>/dev/null; then
                systemctl disable --now "$unit" </dev/null || {
                    echo "无法停止并禁用 $unit，已中止卸载。" >&2
                    exec 9<&-
                    return 1
                }
            fi
        done
    fi

    rm -f -- \
        /etc/systemd/system/p2premote-host.service \
        /etc/systemd/system/p2premote-cloud-host.service \
        /etc/p2premote/host.unattended \
        /etc/p2premote/host.conf \
        /etc/p2premote/cloud-host.conf \
        /etc/p2premote/host.env \
        /etc/p2premote/host.enabled \
        /etc/p2premote/host.changing
    if [[ -d /etc/p2premote ]]; then
        rmdir -- /etc/p2premote 2>/dev/null || true
    fi

    rm -rf -- \
        /usr/local/libexec/p2premote \
        /usr/local/share/p2premote
    rm -f -- \
        /usr/local/bin/p2premote \
        /usr/local/share/applications/p2premote.desktop \
        /etc/xdg/autostart/p2premote-clipboard.desktop

    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload </dev/null 2>/dev/null || true
    fi
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database /usr/local/share/applications >/dev/null 2>&1 </dev/null || true
    fi

    exec 9<&-
    echo "已卸载 P2P Remote 系统安装与被控服务（用户配置 ~/.config/P2PRemote 已保留）。"
}

main() {
    local archive_path=""
    local req_version=""
    local repo="$DEFAULT_REPO"
    local mirror=""
    local manifest_url=""
    local pubkey_arg=""
    local do_uninstall=0

    while (( $# > 0 )); do
        case "$1" in
            -h|--help)
                usage
                return 0
                ;;
            --archive)
                (( $# >= 2 )) || { echo "缺少 --archive 参数值。" >&2; return 2; }
                archive_path=$2
                shift 2
                ;;
            --version)
                (( $# >= 2 )) || { echo "缺少 --version 参数值。" >&2; return 2; }
                req_version=${2#v}
                shift 2
                ;;
            --repo)
                (( $# >= 2 )) || { echo "缺少 --repo 参数值。" >&2; return 2; }
                repo=$2
                shift 2
                ;;
            --mirror)
                (( $# >= 2 )) || { echo "缺少 --mirror 参数值。" >&2; return 2; }
                mirror=$2
                shift 2
                ;;
            --manifest-url)
                (( $# >= 2 )) || { echo "缺少 --manifest-url 参数值。" >&2; return 2; }
                manifest_url=$2
                shift 2
                ;;
            --pubkey)
                (( $# >= 2 )) || { echo "缺少 --pubkey 参数值。" >&2; return 2; }
                pubkey_arg=$2
                shift 2
                ;;
            --uninstall)
                do_uninstall=1
                shift
                ;;
            *)
                echo "未知选项: $1" >&2
                usage >&2
                return 2
                ;;
        esac
    done

    if (( do_uninstall )) && [[ -n $archive_path || -n $req_version || -n $manifest_url || -n $mirror || -n $pubkey_arg ]]; then
        echo "--uninstall 不能与安装源选项同时使用。" >&2
        return 2
    fi

    if [[ -n $req_version && ! $req_version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "版本号格式无效: $req_version" >&2
        return 2
    fi
    if [[ ! $repo =~ ^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$ ]]; then
        echo "GitHub 仓库格式无效: $repo" >&2
        return 2
    fi
    if [[ -n $mirror ]]; then
        while [[ $mirror == */ ]]; do mirror=${mirror%/}; done
        if [[ ! $mirror =~ ^https://[^[:space:]]+$ ]]; then
            echo "镜像地址必须以 https:// 开头: $mirror" >&2
            return 2
        fi
    fi
    if [[ -n $manifest_url && ! $manifest_url =~ ^(https://|http://(127\.0\.0\.1|localhost)(:[0-9]+)?/)[^[:space:]]*$ ]]; then
        echo "清单地址不安全或格式无效: $manifest_url" >&2
        return 2
    fi

    local resolved_pubkey="$DEFAULT_PUBLIC_KEY"
    if [[ -n $pubkey_arg ]]; then
        if [[ -f $pubkey_arg ]]; then
            resolved_pubkey=$(tr -d '\r\n[:space:]' < "$pubkey_arg")
        else
            resolved_pubkey=$(printf '%s' "$pubkey_arg" | tr -d '\r\n[:space:]')
        fi
    fi

    if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
        cat >&2 <<'EOF'
P2P Remote 需要管理员权限安装到 /usr/local，请使用 sudo 运行：
  sudo bash install.sh
或在线安装：
  curl -fsSL https://github.com/qisewai/p2premote-linux/releases/latest/download/install.sh | sudo bash
EOF
        return 1
    fi

    if [[ -n $archive_path ]]; then
        if [[ ! -f $archive_path ]]; then
            echo "找不到指定安装包: $archive_path" >&2
            return 1
        fi
        archive_path=$(readlink -f -- "$archive_path")
    fi

    export PATH=/usr/sbin:/usr/bin:/sbin:/bin
    umask 022
    trap cleanup_temp_work EXIT
    trap 'exit 1' HUP INT TERM

    if (( do_uninstall )); then
        uninstall_system
        return 0
    fi

    if [[ $(uname -m) != x86_64 ]]; then
        echo "当前安装包仅支持 Linux x86_64 架构。" >&2
        return 1
    fi

    local script_path="${BASH_SOURCE[0]:-}"
    local script_dir=""
    if [[ -n $script_path && -f $script_path ]]; then
        script_dir=$(dirname -- "$(readlink -f -- "$script_path")")
    fi

    if [[ -n $archive_path ]]; then
        require_cmd tar
        TEMP_WORK=$(mktemp -d)
        chmod 0700 -- "$TEMP_WORK"
        local unpacked="$TEMP_WORK/unpacked"
        extract_archive_to_temp "$archive_path" "$unpacked"
        install_from_root "$unpacked"
        return 0
    fi

    if [[ -n $manifest_url || -n $req_version || -n $mirror ]]; then
        install_online "$repo" "$req_version" "$mirror" "$manifest_url" "$resolved_pubkey"
        return 0
    fi

    if [[ -n $script_dir && \
          -x "$script_dir/usr/local/libexec/p2premote/p2premote_linux_host" && \
          -x "$script_dir/usr/local/libexec/p2premote/gui/p2premote" ]]; then
        install_from_root "$script_dir"
        return 0
    fi

    if [[ -n $script_dir ]]; then
        local matches=()
        shopt -s nullglob
        matches=("$script_dir"/p2premote-*-linux-x64.tar.gz)
        shopt -u nullglob
        if (( ${#matches[@]} == 1 )); then
            require_cmd tar
            TEMP_WORK=$(mktemp -d)
            chmod 0700 -- "$TEMP_WORK"
            local unpacked="$TEMP_WORK/unpacked"
            extract_archive_to_temp "${matches[0]}" "$unpacked"
            install_from_root "$unpacked"
            return 0
        elif (( ${#matches[@]} > 1 )); then
            echo "当前目录存在多个安装包，请使用 --archive <路径> 指定。" >&2
            return 2
        fi
    fi

    install_online "$repo" "" "" "" "$resolved_pubkey"
}

main "$@"
