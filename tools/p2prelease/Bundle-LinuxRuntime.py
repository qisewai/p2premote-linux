#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


# Keep the dynamic loader, systemd, and the graphics driver stack on the host.
SYSTEM_LIBRARIES = re.compile(
    r"^(?:ld-linux-.*|lib(?:c|m|mvec|dl|pthread|rt|resolv|util|anl|gcc_s|stdc\+\+"
    r"|systemd|udev|GL|EGL|GLX|GLdispatch|OpenGL|GLU|glapi|gbm|drm(?:_[^.]*)?"
    r"|vulkan|va(?:-drm|-x11|-wayland)?|xcb(?:-dri[23])?|X11(?:-xcb)?))\.so(?:\..*)?$"
)
RESOLVED = re.compile(r"^\s+(\S+) => (/\S+)")


def elf_files(root):
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as file:
            if file.read(4) == b"\x7fELF":
                yield path


def bundle(runtime):
    common_libs = runtime / "lib"
    gui = runtime / "gui"
    gui_libs = gui / "lib"
    library_paths = (gui_libs, common_libs)
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = ":".join(map(str, library_paths))
    binaries = list(elf_files(runtime))

    # Qt opens OpenSSL with dlopen. Bundle the matching libssl even if no
    # deployed ELF declares it as a link-time dependency.
    ssl = common_libs / "libssl.so.3"
    if not ssl.exists():
        ssl_dependencies = subprocess.check_output(["ldd", shutil.which("openssl")], text=True)
        ssl_sources = [
            Path(match[2])
            for line in ssl_dependencies.splitlines()
            if (match := RESOLVED.match(line)) and match[1] == "libssl.so.3"
        ]
        if not ssl_sources:
            raise SystemExit("OpenSSL 3 is required on the build machine.")
        shutil.copy2(ssl_sources[0], ssl)
        binaries.append(ssl)

    # ldd provides the baseline distro's resolved paths, including libraries in
    # directories such as pulseaudio/ that aren't under the app's lib directories.
    locations = {}
    for path in binaries:
        output = subprocess.check_output(["ldd", str(path)], env=environment, text=True)
        for line in output.splitlines():
            match = RESOLVED.match(line)
            if match:
                locations.setdefault(match[1], Path(match[2]))

    for path in binaries:
        needed = subprocess.check_output(["patchelf", "--print-needed", str(path)], text=True)
        for name in needed.splitlines():
            if "/" in name or name in {".", ".."}:
                raise SystemExit(f"Non-portable dependency in {path.name}: {name}")
            if SYSTEM_LIBRARIES.fullmatch(name) or any((folder / name).is_file() for folder in library_paths):
                continue
            source = locations.get(name)
            if source is None:
                raise SystemExit(f"Missing dependency of {path.name}: {name}")
            destination = common_libs / name
            shutil.copy2(source, destination)
            binaries.append(destination)

    # Older portable updaters install only gui/. Keep its libraries self-contained
    # and let the host use relative symlinks for the files it shares with the GUI.
    for path in sorted(common_libs.iterdir()):
        destination = gui_libs / path.name
        if destination.exists() or destination.is_symlink():
            continue
        path.rename(destination)
        path.symlink_to(os.path.relpath(destination, path.parent))

    # RUNPATH is evaluated independently for each executable, library and Qt plugin.
    binaries = list(elf_files(runtime))
    for path in binaries:
        folder = gui_libs if path.is_relative_to(gui) else common_libs
        rpath = "$ORIGIN/" + os.path.relpath(folder, path.parent)
        subprocess.run(["patchelf", "--set-rpath", rpath, str(path)], check=True)
    environment.pop("LD_LIBRARY_PATH", None)
    for path in binaries:
        output = subprocess.check_output(["ldd", str(path)], env=environment, text=True)
        if "=> not found" in output:
            raise SystemExit(f"Unresolved library in {path}:\n{output}")


if __name__ == "__main__":
    bundle(Path(sys.argv[1]).resolve(strict=True))
