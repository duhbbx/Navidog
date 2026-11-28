#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${1:-}" == --gui ]]; then
  : "${NAVIDOG_OHOS_QT_ROOT:?Set NAVIDOG_OHOS_QT_ROOT to the Qt for HarmonyOS 6.12 prefix}"
  : "${NAVIDOG_SQLITE_SOURCE:?Set NAVIDOG_SQLITE_SOURCE to an SQLite amalgamation}"
  : "${NAVIDOG_OHOS_OPENSSL_ROOT:?Set NAVIDOG_OHOS_OPENSSL_ROOT to the HarmonyOS static OpenSSL prefix}"
  build_dir="${NAVIDOG_BUILD_DIR:-$root/build-harmonyos-gui}"
  package_dir="${NAVIDOG_PACKAGE_DIR:-$root/dist/harmonyos}"
  "$NAVIDOG_OHOS_QT_ROOT/bin/qt-cmake" -S "$root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DNAVIDOG_HARMONYOS=ON -DBUILD_TESTING=OFF \
    -DNAVIDOG_SQLITE_SOURCE="$NAVIDOG_SQLITE_SOURCE" \
    -DOPENSSL_ROOT_DIR="$NAVIDOG_OHOS_OPENSSL_ROOT" -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DNAVIDOG_WITH_POSTGRES=OFF -DNAVIDOG_WITH_MYSQL=OFF \
    -DNAVIDOG_WITH_ORACLE=OFF -DNAVIDOG_WITH_SQLSERVER=OFF
  cmake --build "$build_dir" --target navidog_make_hap --parallel "${NAVIDOG_JOBS:-4}"
  mkdir -p "$package_dir"
  python3 - "$build_dir" "$package_dir" <<'PYTHON'
from pathlib import Path
from shutil import copy2
from sys import argv

hap_files = list(Path(argv[1]).rglob("*.hap"))
signed = [path for path in hap_files if path.stem.endswith("-signed")]
if len(signed) == 1:
    hap_files = signed
if len(hap_files) != 1:
    raise SystemExit(f"Expected a single Navidog HAP, found {len(hap_files)} in {argv[1]}")
copy2(hap_files[0], Path(argv[2]) / "Navidog-HarmonyOS.hap")
PYTHON
  exit 0
fi

: "${NAVIDOG_OHOS_NATIVE:?Set NAVIDOG_OHOS_NATIVE to the HarmonyOS SDK native directory}"
toolchain="$NAVIDOG_OHOS_NATIVE/build/cmake/ohos.toolchain.cmake"
arch="${1:-arm64-v8a}"
case "$arch" in arm64-v8a|x86_64) ;; *) echo "Unsupported HarmonyOS architecture: $arch" >&2; exit 2 ;; esac
build_dir="${NAVIDOG_BUILD_DIR:-$root/build-harmonyos-$arch}"
package_dir="${NAVIDOG_PACKAGE_DIR:-$root/dist/harmonyos}"
if [[ ! -f "$toolchain" ]]; then
  echo "HarmonyOS NDK CMake toolchain not found at $toolchain" >&2
  exit 1
fi
cmake -S "$root" -B "$build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DOHOS_ARCH="$arch" -DOHOS_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DNAVIDOG_COMPILER_ONLY=ON -DBUILD_TESTING=OFF
cmake --build "$build_dir" --target navidog-sql --parallel "${NAVIDOG_JOBS:-4}"
mkdir -p "$package_dir"
tar -czf "$package_dir/Navidog-SQL-HarmonyOS-$arch.tar.gz" -C "$build_dir/bin" navidog-sql
