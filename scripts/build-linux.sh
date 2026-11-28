#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
target="${1:?Usage: build-linux.sh ubuntu|debian|archlinux|uos|kylin}"
. /etc/os-release
case "$target:$ID" in
  ubuntu:ubuntu|debian:debian) generator=DEB ;;
  archlinux:arch) generator=ARCH ;;
  uos:uos|uos:uniontech|kylin:kylin|kylin:kylinlinux|kylin:openkylin|kylin:neokylin)
    if command -v dpkg-shlibdeps >/dev/null; then generator=DEB
    elif command -v rpmbuild >/dev/null; then generator=RPM
    else echo 'Install dpkg-dev or the RPM build tools on the target runner.' >&2; exit 2
    fi ;;

  *) echo "The $target build must run on its target distribution (found $ID)." >&2; exit 2 ;;
esac
shift
version_tag="${VERSION_ID:-rolling}"
arch="$(uname -m)"
build_dir="${NAVIDOG_BUILD_DIR:-$root/build-$target}"
package_dir="${NAVIDOG_PACKAGE_DIR:-$root/dist/$target}"
mkdir -p "$package_dir"
cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$build_dir" --parallel "${NAVIDOG_JOBS:-4}"
QT_QPA_PLATFORM=offscreen "$build_dir/navidog" --version
QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir" --parallel "${NAVIDOG_JOBS:-4}" --output-on-failure
version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build_dir/CMakeCache.txt")"
if [[ "$target" == ubuntu && "$arch" == x86_64 ]]; then
  python3 "$root/scripts/package-driver-components.py" "$build_dir" "$package_dir" linux-x86_64
fi
python3 "$root/scripts/package-linux.py" portable "$build_dir" "$package_dir" "$version-$target-$version_tag-$arch"
if [[ "$generator" == ARCH ]]; then
  python3 "$root/scripts/package-linux.py" arch "$build_dir" "$package_dir" "$version"
else
  cpack --config "$build_dir/CPackConfig.cmake" -G "$generator" -B "$package_dir" \
    -D "CPACK_SYSTEM_NAME=$target-$version_tag-$arch"
fi
