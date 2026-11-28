#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
windows_version="${1:-7}"
case "$windows_version" in 7|8) ;; *) echo 'Usage: build-win7-macos.sh [7|8]' >&2; exit 2 ;; esac
build_dir="$project_dir/build-win${windows_version}-msvcrt"
sdk_root="${NAVIDOG_SDK_ROOT:-$HOME/Library/Developer/Navidog}"
export NAVIDOG_WIN_QT="$sdk_root/Qt/5.15.2/mingw81_64"
export NAVIDOG_WIN_DEPS="$sdk_root/win7-deps"
export NAVIDOG_LEGACY_MINGW_ROOT="$sdk_root/Qt/Tools/mingw810_64"
if [[ ! -f "$NAVIDOG_LEGACY_MINGW_ROOT/lib/gcc/x86_64-w64-mingw32/8.1.0/libstdc++.a" ]]; then
  echo 'Install the official Qt MinGW 8.1 x64 tools before building the Win7 package.' >&2
  exit 1
fi
cmake -S "$project_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$project_dir/cmake/toolchains/mingw-w64.cmake" \
  -DNAVIDOG_WIN7=ON -DNAVIDOG_LEGACY_WINDOWS_VERSION="$windows_version" -DCMAKE_BUILD_TYPE=Release \
  -DNAVIDOG_HOST_MOC="${NAVIDOG_HOST_MOC:-/opt/homebrew/opt/qt@5/bin/moc}" \
  -DCMAKE_PREFIX_PATH="$NAVIDOG_WIN_QT" \
  -DNAVIDOG_SQLITE_SOURCE="$NAVIDOG_WIN_DEPS/sqlite-amalgamation-3510000" \
  -DNAVIDOG_WITH_POSTGRES=OFF -DNAVIDOG_WITH_MYSQL=OFF -DNAVIDOG_WITH_ORACLE=OFF \
  -DNAVIDOG_WITH_SQLSERVER=OFF -DNAVIDOG_BUILD_TESTS=OFF -DBUILD_TESTING=OFF
cmake --build "$build_dir" --target navidog --parallel "${NAVIDOG_JOBS:-6}"
python3 "$project_dir/scripts/package-windows7.py" \
  "$build_dir/navidog.exe" "$NAVIDOG_WIN_QT" "$NAVIDOG_LEGACY_MINGW_ROOT" "$windows_version"
python3 "$project_dir/scripts/package-windows-setup.py" "$windows_version" "${NAVIDOG_MAKENSIS:-makensis}"
