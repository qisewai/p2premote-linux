"""Run the installed host helper in private mount, user and network namespaces."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile


def enter_namespace(binary, directory):
    unshare = ["unshare", "--map-root-user", "--mount", "--net", "--propagation", "private"]
    if not shutil.which("unshare") or subprocess.run(unshare + ["true"]).returncode:
        return 77
    directory = Path(directory)
    host = directory / binary.name
    shutil.copy2(binary, host)
    (directory / "lib").symlink_to(binary.parent / "lib")
    units = directory / "units"
    units.mkdir(mode=0o755)
    units.chmod(0o755)
    unit = "p2premote-host.service"
    shutil.copyfile(binary.parent / unit, units / unit)
    (units / unit).chmod(0o644)
    ctl = directory / "systemctl"
    ctl.write_text("""#!/bin/sh
set -eu
case "$1" in
daemon-reload) exit 0 ;;
is-enabled) test -e "/run/$3.enabled" ;;
is-active) test -e "/run/$3.active" || exit 3 ;;
enable) touch "/run/$3.enabled" ;;
start|restart) test ! -e /run/fail-start && touch "/run/$3.active" ;;
disable) test ! -e /run/fail-stop || exit 1
         rm -f "/run/$3.enabled"; if test "$2" = --now; then rm -f "/run/$3.active"; fi ;;
stop) rm -f "/run/$3.active" ;;
*) exit 1 ;;
esac
""")
    ctl.chmod(0o755)
    setup = """
        mount -t tmpfs tmpfs /etc
        mount -t tmpfs tmpfs /run
        mkdir -p /run/lock /etc/systemd/system
        mount --bind "$1" /usr/bin/systemctl
        ip link set lo up
        exec "$2" "$3" --inside "$4"
    """
    return subprocess.run(unshare + ["sh", "-ec", setup, "test", str(ctl),
                                     sys.executable, __file__, str(host)],
                          env=dict(os.environ, TEST_PARENT_MNT=os.readlink("/proc/self/ns/mnt"),
                                   TEST_PARENT_NET=os.readlink("/proc/self/ns/net"))).returncode


def exercise(host):
    config = Path("/etc/p2premote")
    units = Path("/etc/systemd/system")
    def run(command, payload="", success=True):
        rc = subprocess.run([host, command], input=payload.encode()).returncode
        assert (rc == 0) == success, (command, rc)

    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen()
    server.settimeout(4)
    device = "D" * 24
    cloud = json.dumps(dict(server=f"http://127.0.0.1:{server.getsockname()[1]}", user="test",
                            password="test-fixture", installationId="I" * 43, deviceId=device))

    # A failing second step must restore a clean installation.
    Path("/run/fail-start").touch()
    run("--install-host-service", cloud, success=False)
    assert not list(units.iterdir()) and not list(config.iterdir())
    assert not list(Path("/run").glob("*.enabled"))
    Path("/run/fail-start").unlink()

    run("--install-host-service", cloud)
    assert (config / "host.enabled").exists() and not (config / "host.unattended").exists()
    before = (config / "cloud-host.conf").read_bytes()
    run("--enable-service", "43888\nfixture-code\n")
    lan = (config / "host.conf").read_bytes()
    run("--enable-unattended")
    assert (config / "host.unattended").exists()

    # Release an HTTP response after the foreground authorization has ended.
    # The service must stay disconnected, then log in again on a fresh grant.
    env = dict(os.environ, LOGS_DIRECTORY=str(host.parent), NO_PROXY="127.0.0.1")
    status = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    status.bind("/run/notify")
    status.settimeout(4)
    env["NOTIFY_SOCKET"] = "/run/notify"
    process = subprocess.Popen([host, "--system-service"], env=env)
    try:
        with server.accept()[0] as login:
            assert b"POST /api/v2/login" in login.recv(4096)
            run("--disable-unattended")
            while b"cloud=standby:" not in status.recv(1024):
                pass
            result = json.dumps(dict(accessToken="A"*43, currentDeviceId=device, iceServers=[])).encode()
            try:
                login.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: "
                              + str(len(result)).encode() + b"\r\n\r\n" + result)
            except (BrokenPipeError, ConnectionResetError):
                pass
        server.settimeout(0.8)
        try:
            sock, _ = server.accept()
        except TimeoutError:
            pass
        else:
            sock.close()
            raise AssertionError("standby reopened a network connection")
        server.settimeout(4)
        run("--enable-unattended")
        with server.accept()[0] as fresh:
            assert b"POST /api/v2/login" in fresh.recv(4096)
    finally:
        process.terminate()
        process.wait(timeout=5)
        status.close()
        server.close()

    assert (config / "cloud-host.conf").read_bytes() == before
    assert (config / "host.conf").read_bytes() == lan
    Path("/run/fail-stop").touch()
    run("--uninstall-host-service", success=False)
    assert (config / "host.inactive").exists() and (config / "host.enabled").exists()
    assert (config / "cloud-host.conf").read_bytes() == before and len(list(units.iterdir())) == 1
    Path("/run/fail-stop").unlink()
    run("--uninstall-host-service")
    run("--uninstall-host-service")
    assert not config.exists() and not list(units.iterdir())

    # Persist the consent of a legacy LAN installation before disabling its last channel.
    config.mkdir(mode=0o700)
    Path("/run/p2premote-host.service.enabled").touch()
    run("--disable-service")
    assert (config / "host.enabled").exists() and not (config / "host.unattended").exists()


if __name__ == "__main__":
    if sys.argv[1] == "--inside":
        host = Path(sys.argv[2])
        if (os.geteuid() != 0 or not os.path.samefile("/usr/bin/systemctl", host.parent / "systemctl")
                or os.readlink("/proc/self/ns/mnt") == os.environ["TEST_PARENT_MNT"]
                or os.readlink("/proc/self/ns/net") == os.environ["TEST_PARENT_NET"]):
            sys.exit("service test requires its private namespaces and systemctl fixture")
        exercise(host)
    else:
        with tempfile.TemporaryDirectory(prefix="p2premote-host-test-") as tmp:
            sys.exit(enter_namespace(Path(sys.argv[1]).resolve(), tmp))
