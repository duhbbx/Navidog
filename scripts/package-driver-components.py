#!/usr/bin/env python3
"""Create release assets separate from the Navidog application packages."""
import argparse
import hashlib
import re
import tarfile
from pathlib import Path


def write_asset(source: Path, paths: list[Path], output: Path) -> None:
    with tarfile.open(output, "w:gz", compresslevel=6, dereference=True) as archive:
        for path in paths:
            archive.add(path, arcname=path.relative_to(source))
    with output.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    output.with_suffix(output.suffix + ".sha256").write_text(
        f"{digest}  {output.name}\n", encoding="ascii"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("platform", choices=("macos-arm64", "macos-x86_64", "windows-x86_64", "linux-x86_64"))
    args = parser.parse_args()
    cache = (args.build / "CMakeCache.txt").read_text(encoding="utf-8")
    version = re.search(r"^CMAKE_PROJECT_VERSION:STATIC=(\d+\.\d+\.\d+)$", cache, re.MULTILINE)
    if version is None:
        parser.error("build version missing from CMakeCache.txt")
    args.output.mkdir(parents=True, exist_ok=True)
    stem = f"Navidog-{version[1]}"
    runtime = args.build / "jdbc" / "runtime"
    java = "java.exe" if args.platform.startswith("windows") else "java"
    if (runtime / "bin" / java).is_file() and (runtime / "release").is_file():
        write_asset(runtime, sorted(runtime.iterdir()), args.output / f"{stem}-JRE-{args.platform}.tar.gz")
    redis = args.build / "driver-runtime" / "redis"
    library = {
        "macos": "libnavidog-hiredis.dylib",
        "windows": "navidog-hiredis.dll",
        "linux": "libnavidog-hiredis.so",
    }[args.platform.split("-")[0]]
    if (redis / library).is_file() and (redis / "COPYING").is_file():
        write_asset(redis, [redis / library, redis / "COPYING"],
                    args.output / f"{stem}-Driver-Redis-{args.platform}.tar.gz")


if __name__ == "__main__":
    main()
