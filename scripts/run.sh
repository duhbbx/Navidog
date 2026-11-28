#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
qt_prefix="${NAVIDOG_QT_PREFIX:-}"
if [[ -z "$qt_prefix" ]] && command -v qtpaths >/dev/null 2>&1; then
  qt_prefix="$(qtpaths --query QT_INSTALL_PREFIX)"
fi
cmake_args=(-S "$project_dir" -B "$project_dir/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo)
if [[ -n "$qt_prefix" ]]; then
  cmake_args+=("-DCMAKE_PREFIX_PATH=$qt_prefix")
fi
if command -v ninja >/dev/null 2>&1 && [[ ! -f "$project_dir/build/CMakeCache.txt" ]]; then
  cmake_args+=(-G Ninja)
fi
cmake "${cmake_args[@]}"
cmake --build "$project_dir/build" --parallel
if [[ "$(uname -s)" == "Darwin" ]]; then
  open -n "$project_dir/build/navidog.app" --args "$@"
else
  exec "$project_dir/build/navidog" "$@"
fi
