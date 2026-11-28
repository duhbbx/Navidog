#!/usr/bin/env python3
"""Package a native Linux build as an unpack-and-run bundle or a pacman package."""
import os
from pathlib import Path
import re
import shutil
import subprocess
from tempfile import TemporaryDirectory


def checked_output(*args):
    return subprocess.check_output(args, text=True).strip()


def linked_libraries(binary):
    for line in checked_output("ldd", binary).splitlines():
        if "=> not found" in line:
            raise SystemExit(f"{binary.name}: {line.strip()}")
        match = re.search(r"(?:=>\s+|^\s*)(/[^ ]+)\s+\(", line)
        if match:
            yield Path(match[1])


def portable(build_dir, package_dir, label):
    qmake = Path((build_dir / "qt-qmake-path.txt").read_text().strip())
    qt_plugins = Path(checked_output(qmake, "-query", "QT_INSTALL_PLUGINS"))
    qt_translations = Path(checked_output(qmake, "-query", "QT_INSTALL_TRANSLATIONS"))
    xcb = qt_plugins / "platforms/libqxcb.so"
    if not xcb.is_file():
        raise SystemExit(f"Linux Qt package needs the XCB platform plugin: {xcb}")
    with TemporaryDirectory(prefix="navidog-portable-") as work:
        app = Path(work) / "Navidog"
        for name in ("bin", "lib", "plugins/platforms", "translations"):
            (app / name).mkdir(parents=True, exist_ok=True)
        source_root = Path(__file__).resolve().parents[1]
        for name in ("LICENSE", "NOTICE"):
            shutil.copy2(source_root / name, app / name)
        shutil.copy2(source_root / "resources/branding/navidog-256.png", app / "navidog.png")
        executable = build_dir / "navidog"
        shutil.copy2(executable, app / "bin/navidog")
        jdbc = build_dir / "jdbc"
        if "NAVIDOG_WITH_JDBC:BOOL=ON" in (build_dir / "CMakeCache.txt").read_text():
            target = app / "lib/navidog/jdbc"
            target.mkdir(parents=True, exist_ok=True)
            shutil.copy2(jdbc / "navidog-jdbc-bridge.jar", target)
        libraries = set(linked_libraries(executable))
        for name in ("libqxcb.so", "libqoffscreen.so", "libqwayland-egl.so", "libqwayland-generic.so", "libqwayland.so"):
            plugin = qt_plugins / "platforms" / name
            if plugin.is_file():
                shutil.copy2(plugin, app / "plugins/platforms")
                libraries.update(linked_libraries(plugin))
        for category in ("imageformats", "tls", "xcbglintegrations", "wayland-graphics-integration-client"):
            directory = qt_plugins / category
            if directory.is_dir():
                (app / "plugins" / category).mkdir()
                for plugin in directory.glob("*.so"):
                    shutil.copy2(plugin, app / "plugins" / category)
                    libraries.update(linked_libraries(plugin))
        if (qt_translations / "qtbase_zh_CN.qm").exists():
            shutil.copy2(qt_translations / "qtbase_zh_CN.qm", app / "translations")
        for library in libraries:
            # The portable archive uses the distribution's system loader and glibc.
            if not re.match(r"^(?:ld-linux|ld64|lib(?:c|m|mvec|dl|pthread|rt|resolv|util|anl|nss_[^.]*)\.so)", library.name):
                shutil.copy2(library, app / "lib" / library.name)
        (app / "bin/qt.conf").write_text("[Paths]\nPrefix=..\n")
        launcher = app / "Navidog"
        launcher.write_text(
            '#!/bin/sh\n'
            'root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
            'export LD_LIBRARY_PATH="$root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
            'exec "$root/bin/navidog" "$@"\n'
        )
        launcher.chmod(0o755)
        subprocess.run([launcher, "--version"], env=dict(os.environ, QT_QPA_PLATFORM="offscreen"), check=True)
        subprocess.run(["tar", "-czf", package_dir / f"Navidog-{label}-Portable.tar.gz", "-C", work, "Navidog"], check=True)


def arch_package(build_dir, package_dir, version):
    arch = checked_output("uname", "-m")
    with TemporaryDirectory(prefix="navidog-arch-") as work:
        stage = Path(work)
        subprocess.run(["cmake", "--install", build_dir, "--prefix", stage / "root/usr"], check=True)
        depends = sorted({checked_output("pacman", "-Qqo", str(path.resolve()))
                          for path in linked_libraries(build_dir / "navidog")})
        metadata = [
            "pkgname = navidog", "pkgbase = navidog", f"pkgver = {version}-1",
            "pkgdesc = Database workbench", f"arch = {arch}",
            "packager = Navidog", "license = Apache-2.0", f"size = {(stage / 'root/usr/bin/navidog').stat().st_size}",
        ]
        metadata.extend(f"depend = {package}" for package in depends)
        (stage / "root/.PKGINFO").write_text("\n".join(metadata) + "\n")
        package = package_dir / f"navidog-{version}-1-{arch}.pkg.tar.zst"
        subprocess.run(["bsdtar", "--uid", "0", "--gid", "0", "--zstd", "-cf", package,
                        "-C", stage / "root", ".PKGINFO", "usr"], check=True)
        subprocess.run(["pacman", "-Qip", package], check=True)


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("format", choices=["portable", "arch"])
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("package_dir", type=Path)
    parser.add_argument("label")
    args = parser.parse_args()
    if args.format == "portable":
        portable(args.build_dir, args.package_dir, args.label)
    else:
        arch_package(args.build_dir, args.package_dir, args.label)
