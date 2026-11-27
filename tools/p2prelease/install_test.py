"""Run tools/p2prelease/install.sh inside private mount, user and network namespaces."""
import http.server
import os
from pathlib import Path
import shutil
import socketserver
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading


def enter_namespace(install_sh: Path, p2prelease: Path, work_dir: Path) -> int:
    # 1. Verify non-root execution fails cleanly before entering namespace.
    if os.geteuid() != 0:
        res = subprocess.run(["bash", str(install_sh)], capture_output=True, text=True)
        assert res.returncode != 0, "expected non-root execution to fail"
        assert "sudo" in res.stderr, f"expected sudo guidance in stderr, got: {res.stderr!r}"

    unshare = ["unshare", "--map-root-user", "--mount", "--net", "--propagation", "private"]
    if not shutil.which("unshare") or subprocess.run(unshare + ["true"],
                                                    stdout=subprocess.DEVNULL,
                                                    stderr=subprocess.DEVNULL).returncode:
        return 77

    ctl = work_dir / "systemctl"
    ctl.write_text("""#!/bin/sh
set -eu
echo "$*" >> /run/systemctl.log
case "$1" in
daemon-reload)
    exit 0
    ;;
is-enabled)
    unit=${3:-$2}
    test -e "/run/$unit.enabled"
    ;;
is-active)
    unit=${3:-$2}
    test -e "/run/$unit.active" || exit 3
    ;;
enable)
    touch "/run/$2.enabled"
    ;;
start|restart)
    unit=$2
    if [ -e /run/fail-start-once ]; then
        rm -f /run/fail-start-once
        exit 1
    fi
    if [ -e /run/fail-start ]; then
        exit 1
    fi
    touch "/run/$unit.active"
    ;;
disable)
    if [ "$2" = "--now" ]; then
        unit=$3
        rm -f "/run/$unit.enabled" "/run/$unit.active"
    else
        rm -f "/run/$2.enabled"
    fi
    ;;
stop)
    shift
    for unit in "$@"; do
        rm -f "/run/$unit.active"
    done
    ;;
*)
    exit 1
    ;;
esac
""", encoding="utf-8", newline="\n")
    ctl.chmod(0o755)

    awk_real = os.path.realpath(shutil.which("awk") or "")
    setup = """
        mount -t tmpfs -o mode=0755 tmpfs /usr/local
        mount -t tmpfs -o mode=0755 tmpfs /etc
        mount -t tmpfs -o mode=0755 tmpfs /run
        mkdir -p /run/lock /etc/systemd/system /etc/xdg/autostart /usr/local/bin /usr/local/libexec /usr/local/share/applications
        if [ -n "$7" ] && [ ! -e /usr/bin/awk ]; then
            mkdir -p /etc/alternatives
            ln -s "$7" /etc/alternatives/awk
        fi
        mount --bind "$1" /usr/bin/systemctl
        ip link set lo up
        exec "$2" "$3" --inside "$4" "$5" "$6"
    """
    return subprocess.run(
        unshare + [
            "sh", "-ec", setup, "test",
            str(ctl), sys.executable, __file__,
            str(install_sh), str(p2prelease), str(work_dir), awk_real,
        ],
        env=dict(
            os.environ,
            TEST_PARENT_MNT=os.readlink("/proc/self/ns/mnt"),
            TEST_PARENT_NET=os.readlink("/proc/self/ns/net"),
        ),
    ).returncode


def make_payload(stage: Path, version: str, install_sh_src: Path, pubkey_b64: str) -> None:
    runtime = stage / "usr/local/libexec/p2premote"
    (runtime / "gui").mkdir(parents=True, exist_ok=True)
    (runtime / "lib").mkdir(parents=True, exist_ok=True)
    (runtime / "units").mkdir(parents=True, exist_ok=True)
    (stage / "usr/local/bin").mkdir(parents=True, exist_ok=True)
    (stage / "usr/local/share/p2premote/plugins").mkdir(parents=True, exist_ok=True)
    (stage / "usr/local/share/applications").mkdir(parents=True, exist_ok=True)
    (stage / "etc/xdg/autostart").mkdir(parents=True, exist_ok=True)

    host_script = f"""#!/bin/sh
set -eu
if [ "${{1:-}}" = "--uninstall-host-service" ]; then
    flock /run/lock/p2premote-host.lock sh -c '
        systemctl disable --now p2premote-host.service || true
        rm -f /etc/systemd/system/p2premote-host.service
        rm -rf /etc/p2premote
        systemctl daemon-reload || true
        echo "uninstalled-{version}" > /run/host-uninstalled
    '
    exit 0
fi
echo "{version}"
"""
    (runtime / "p2premote_linux_host").write_text(host_script, encoding="utf-8", newline="\n")
    (runtime / "p2premote_linux_host").chmod(0o755)

    (runtime / "gui/p2premote").write_text(f"#!/bin/sh\necho gui-{version}\n", encoding="utf-8", newline="\n")
    (runtime / "gui/p2premote").chmod(0o755)
    (runtime / "p2premote_clipboard").write_text(f"#!/bin/sh\necho clip-{version}\n", encoding="utf-8", newline="\n")
    (runtime / "p2premote_clipboard").chmod(0o755)

    (runtime / "lib/libdummy.so").write_text(f"lib-{version}\n", encoding="utf-8", newline="\n")
    (runtime / "lib/libdummy.so.1").symlink_to("libdummy.so")

    (runtime / "units/p2premote-host.service").write_text(
        f"[Unit]\nDescription=P2P Remote {version}\n[Service]\nExecStart=/usr/local/libexec/p2premote/p2premote_linux_host --system-service\n",
        encoding="utf-8", newline="\n",
    )
    (runtime / "units/p2premote-host.service").chmod(0o644)

    (stage / "usr/local/bin/p2premote").write_text(
        f"#!/bin/sh\n# launcher {version}\nexec /usr/local/libexec/p2premote/gui/p2premote \"$@\"\n",
        encoding="utf-8", newline="\n",
    )
    (stage / "usr/local/bin/p2premote").chmod(0o755)
    (stage / "usr/local/share/p2premote/plugins/main.lua").write_text(f"-- {version}\n", encoding="utf-8", newline="\n")
    (stage / "usr/local/share/applications/p2premote.desktop").write_text(
        f"[Desktop Entry]\nName=P2P Remote {version}\nExec=/usr/local/bin/p2premote\n",
        encoding="utf-8", newline="\n",
    )
    (stage / "etc/xdg/autostart/p2premote-clipboard.desktop").write_text(
        f"[Desktop Entry]\nName=Clipboard {version}\nExec=/usr/local/libexec/p2premote/p2premote_clipboard\n",
        encoding="utf-8", newline="\n",
    )
    (stage / "VERSION").write_text(f"{version}\n", encoding="utf-8", newline="\n")

    rendered = install_sh_src.read_text(encoding="utf-8")
    out_lines = []
    for line in rendered.splitlines():
        if line.startswith("DEFAULT_PUBLIC_KEY="):
            out_lines.append(f"DEFAULT_PUBLIC_KEY='{pubkey_b64}'")
        else:
            out_lines.append(line)
    (stage / "install.sh").write_text("\n".join(out_lines) + "\n", encoding="utf-8", newline="\n")
    (stage / "install.sh").chmod(0o755)


def assert_installed_version(expected_version: str) -> None:
    runtime = Path("/usr/local/libexec/p2premote")
    assert (runtime / "p2premote_linux_host").exists()
    out = subprocess.check_output([str(runtime / "gui/p2premote")], text=True).strip()
    assert out == f"gui-{expected_version}", (out, expected_version)
    assert (runtime / "lib/libdummy.so.1").is_symlink()
    assert os.readlink(runtime / "lib/libdummy.so.1") == "libdummy.so"
    assert (runtime / "lib/libdummy.so").read_text().strip() == f"lib-{expected_version}"
    assert Path("/usr/local/share/p2premote/plugins/main.lua").read_text().strip() == f"-- {expected_version}"
    for path in (
        runtime,
        runtime / "p2premote_linux_host",
        runtime / "gui/p2premote",
        Path("/usr/local/bin/p2premote"),
        Path("/usr/local/share/p2premote"),
        Path("/usr/local/share/applications/p2premote.desktop"),
        Path("/etc/xdg/autostart/p2premote-clipboard.desktop"),
    ):
        st = path.lstat()
        assert st.st_uid == 0, (path, st.st_uid)
        if not stat.S_ISLNK(st.st_mode):
            assert (stat.S_IMODE(st.st_mode) & 0o022) == 0, (path, oct(st.st_mode))


def exercise(install_sh: Path, p2prelease: Path, work_dir: Path) -> None:
    keys_dir = work_dir / "keys"
    subprocess.run([str(p2prelease), "keygen", "--out", str(keys_dir)], check=True, stdout=subprocess.DEVNULL)
    priv_key = keys_dir / "p2premote-update.key"
    pub_key = keys_dir / "p2premote-update.pub"
    pub_b64 = pub_key.read_text(encoding="utf-8").strip()

    # Reserve local port for online tests.
    class QuietHandler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

    http_dir = work_dir / "http"
    http_dir.mkdir()
    handler = lambda *args, **kwargs: QuietHandler(*args, directory=str(http_dir), **kwargs)
    httpd = socketserver.TCPServer(("127.0.0.1", 0), handler)
    port = httpd.server_address[1]
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()

    try:
        # 2. Offline extracted-tarball fresh install (0.1.0).
        stage_010 = work_dir / "stage-0.1.0"
        make_payload(stage_010, "0.1.0", install_sh, pub_b64)
        subprocess.run(["bash", str(stage_010 / "install.sh")], check=True)
        assert_installed_version("0.1.0")
        assert not Path("/etc/systemd/system/p2premote-host.service").exists()

        # 3. Symlink escape in payload must be rejected cleanly before touching /usr/local.
        bad_stage = work_dir / "stage-bad"
        make_payload(bad_stage, "9.9.9", install_sh, pub_b64)
        (bad_stage / "usr/local/share/p2premote/escape").symlink_to("../../../etc/passwd")
        bad_res = subprocess.run(["bash", str(bad_stage / "install.sh")], capture_output=True, text=True)
        assert bad_res.returncode != 0, "expected symlink escape to be rejected"
        assert_installed_version("0.1.0")

        # 4. Simulate active host service & config, then upgrade 0.1.0 -> 0.2.0 via --archive.
        etc_cfg = Path("/etc/p2premote")
        etc_cfg.mkdir(mode=0o755, exist_ok=True)
        (etc_cfg / "host.conf").write_text("port=43888\nsecret=keep-me\n", encoding="utf-8", newline="\n")
        (etc_cfg / "host.enabled").write_text("1\n", encoding="utf-8", newline="\n")
        Path("/etc/systemd/system/p2premote-host.service").write_text(
            "[Unit]\nDescription=Old 0.1.0\n", encoding="utf-8", newline="\n"
        )
        Path("/etc/systemd/system/p2premote-host.service").chmod(0o644)
        Path("/run/p2premote-host.service.active").touch()
        Path("/run/p2premote-host.service.enabled").touch()

        stage_020 = work_dir / "stage-0.2.0"
        make_payload(stage_020, "0.2.0", install_sh, pub_b64)
        out_020 = work_dir / "out-0.2.0"
        subprocess.run([
            str(p2prelease), "pack",
            "--dir", str(stage_020),
            "--target", "linux-x64",
            "--version", "0.2.0",
            "--base-url", f"http://127.0.0.1:{port}/",
            "--out", str(out_020),
            "--key", str(priv_key),
        ], check=True, stdout=subprocess.DEVNULL)
        archive_020 = out_020 / "p2premote-0.2.0-linux-x64.tar.gz"
        subprocess.run(["bash", str(stage_020 / "install.sh"), "--archive", str(archive_020)], check=True)
        assert_installed_version("0.2.0")
        assert (etc_cfg / "host.conf").read_text(encoding="utf-8") == "port=43888\nsecret=keep-me\n"
        unit_text = Path("/etc/systemd/system/p2premote-host.service").read_text(encoding="utf-8")
        assert "Description=P2P Remote 0.2.0" in unit_text
        assert Path("/run/p2premote-host.service.active").exists()
        assert "daemon-reload" in Path("/run/systemctl.log").read_text(encoding="utf-8")

        # 5. Upgrade failure during service restart must roll back files, unit, and service state.
        stage_030 = work_dir / "stage-0.3.0"
        make_payload(stage_030, "0.3.0", install_sh, pub_b64)
        subprocess.run([
            str(p2prelease), "pack",
            "--dir", str(stage_030),
            "--target", "linux-x64",
            "--version", "0.3.0",
            "--base-url", f"http://127.0.0.1:{port}/",
            "--out", str(http_dir),
            "--key", str(priv_key),
        ], check=True, stdout=subprocess.DEVNULL)
        archive_030 = http_dir / "p2premote-0.3.0-linux-x64.tar.gz"

        Path("/run/fail-start-once").touch()
        fail_res = subprocess.run(["bash", str(stage_030 / "install.sh"), "--archive", str(archive_030)],
                                  capture_output=True, text=True)
        assert fail_res.returncode != 0, "expected restart failure to abort upgrade"
        assert_installed_version("0.2.0")
        unit_text_after_rb = Path("/etc/systemd/system/p2premote-host.service").read_text(encoding="utf-8")
        assert "Description=P2P Remote 0.2.0" in unit_text_after_rb
        assert (etc_cfg / "host.conf").read_text(encoding="utf-8") == "port=43888\nsecret=keep-me\n"
        assert Path("/run/p2premote-host.service.active").exists()

        # 6. Verify all three parse_manifest backends (jq, python3, awk) ignoring nested deltas/previous.
        sample_manifest = work_dir / "nested-manifest.json"
        sample_manifest.write_text("""{
  "version": "0.3.0",
  "notes": "line1\\nline2",
  "url": "https://example.invalid/top.tar.gz",
  "size": 123456,
  "signature": "TOPSIG==",
  "deltas": [
    {"from": "0.2.0", "url": "https://example.invalid/delta.delta", "size": 999, "signature": "DELTASIG=="}
  ],
  "previous": [
    {"version": "0.2.0", "url": "https://example.invalid/prev.tar.gz", "size": 888, "signature": "PREVSIG=="}
  ],
  "target": "linux-x64"
}
""", encoding="utf-8", newline="\n")
        for blocked in ("jq", "jq|python3"):
            parser_check = f"""
set -euo pipefail
source <(sed '$d' {str(install_sh)!r})
command() {{
    if [[ ${{1:-}} == -v ]] && [[ ${{2:-}} =~ ^({blocked})$ ]]; then
        return 1
    fi
    builtin command "$@"
}}
parse_manifest {str(sample_manifest)!r}
printf '%s|%s|%s|%s|%s\\n' "$MANIFEST_VERSION" "$MANIFEST_URL" "$MANIFEST_SIZE" "$MANIFEST_SIGNATURE" "$MANIFEST_TARGET"
"""
            res = subprocess.run(["bash", "-c", parser_check], check=True, capture_output=True, text=True)
            assert res.stdout.strip() == "0.3.0|https://example.invalid/top.tar.gz|123456|TOPSIG==|linux-x64", (
                blocked, res.stdout
            )

        # 7. Online mode (curl | sudo bash simulation via stdin): tampered archive must fail signature check.
        orig_bytes = archive_030.read_bytes()
        tampered = bytearray(orig_bytes)
        tampered[-10] ^= 0x55
        archive_030.write_bytes(bytes(tampered))
        piped_script = (stage_030 / "install.sh").read_text(encoding="utf-8")
        tamper_res = subprocess.run(
            ["bash", "-s", "--", "--manifest-url", f"http://127.0.0.1:{port}/update-linux-x64.json",
             "--pubkey", str(pub_key)],
            input=piped_script, text=True, capture_output=True,
        )
        assert tamper_res.returncode != 0, "tampered online archive must be rejected"
        assert "签名校验失败" in tamper_res.stderr, tamper_res.stderr
        assert_installed_version("0.2.0")

        # 7. Online mode with valid signed archive upgrades cleanly from 0.2.0 -> 0.3.0.
        archive_030.write_bytes(orig_bytes)
        subprocess.run(
            ["bash", "-s", "--", "--manifest-url", f"http://127.0.0.1:{port}/update-linux-x64.json",
             "--pubkey", str(pub_key)],
            input=piped_script, text=True, check=True,
        )
        assert_installed_version("0.3.0")
        assert "Description=P2P Remote 0.3.0" in Path("/etc/systemd/system/p2premote-host.service").read_text()
        assert Path("/run/p2premote-host.service.active").exists()

        # 8. Uninstall invokes host uninstall without lock deadlock and cleans all 5 targets.
        subprocess.run(
            ["bash", "-s", "--", "--uninstall"],
            input=piped_script, text=True, check=True,
        )
        assert Path("/run/host-uninstalled").read_text().strip() == "uninstalled-0.3.0"
        assert not Path("/usr/local/libexec/p2premote").exists()
        assert not Path("/usr/local/bin/p2premote").exists()
        assert not Path("/usr/local/share/p2premote").exists()
        assert not Path("/usr/local/share/applications/p2premote.desktop").exists()
        assert not Path("/etc/xdg/autostart/p2premote-clipboard.desktop").exists()
        assert not Path("/etc/systemd/system/p2premote-host.service").exists()
        assert not Path("/etc/p2premote").exists()
    finally:
        httpd.shutdown()
        httpd.server_close()


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--inside":
        install_sh = Path(sys.argv[2])
        p2prelease = Path(sys.argv[3])
        work_dir = Path(sys.argv[4])
        if (os.geteuid() != 0
                or not os.path.samefile("/usr/bin/systemctl", work_dir / "systemctl")
                or os.readlink("/proc/self/ns/mnt") == os.environ["TEST_PARENT_MNT"]
                or os.readlink("/proc/self/ns/net") == os.environ["TEST_PARENT_NET"]):
            sys.exit("install test requires private mount/net namespaces and systemctl fixture")
        exercise(install_sh, p2prelease, work_dir)
    else:
        if len(sys.argv) != 3:
            sys.exit("Usage: install_test.py <install.sh> <p2prelease>")
        with tempfile.TemporaryDirectory(prefix="p2premote-install-test-") as tmp:
            sys.exit(enter_namespace(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), Path(tmp)))
