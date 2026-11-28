#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
build_dir="${NAVIDOG_BUILD_DIR:-build}"
package_dir="${NAVIDOG_PACKAGE_DIR:-$project_dir/dist}"
cmake --build "$build_dir" --target navidog --parallel "${NAVIDOG_JOBS:-6}"
stage_dir="$package_dir/Navidog.app"
if [[ -e "$stage_dir" ]]; then rm -rf -- "$stage_dir"; fi
mkdir -p "$package_dir"
ditto "$build_dir/navidog.app" "$stage_dir"
jdbc_bundle="$stage_dir/Contents/Resources/jdbc"
if [[ -d "$jdbc_bundle" ]]; then
  cmake -E rm -rf "$jdbc_bundle"
fi
qtpaths_executable="${NAVIDOG_QTPATHS:-qtpaths}"
plugin_dir="$("$qtpaths_executable" --query QT_INSTALL_PLUGINS)"
qt_lib_dir="$("$qtpaths_executable" --query QT_INSTALL_LIBS)"
mkdir -p "$stage_dir/Contents/PlugIns/platforms" "$stage_dir/Contents/PlugIns/styles" "$stage_dir/Contents/PlugIns/iconengines" "$stage_dir/Contents/PlugIns/tls"
for plugin in platforms/libqcocoa.dylib styles/libqmacstyle.dylib iconengines/libqsvgicon.dylib tls/libqsecuretransportbackend.dylib; do
  ditto "$plugin_dir/$plugin" "$stage_dir/Contents/PlugIns/$plugin"
done
deploy_args=("-libpath=$qt_lib_dir")
# Homebrew distributes QtSvg separately from QtBase.
if [[ -d /opt/homebrew/opt/qtsvg/lib ]]; then
  deploy_args+=("-libpath=/opt/homebrew/opt/qtsvg/lib")
fi
other_binaries=()
obclient="$stage_dir/Contents/Frameworks/libnavidog-obclient.dylib"
if [[ -f "$obclient" ]]; then
  install_name_tool -id "@loader_path/libnavidog-obclient.dylib" "$obclient"
  other_binaries+=("-executable=$obclient")
fi
macdeployqt "$stage_dir" -no-plugins -verbose=1 ${other_binaries[@]+"${other_binaries[@]}"} \
  "-executable=$stage_dir/Contents/PlugIns/platforms/libqcocoa.dylib" \
  "-executable=$stage_dir/Contents/PlugIns/styles/libqmacstyle.dylib" \
  "-executable=$stage_dir/Contents/PlugIns/iconengines/libqsvgicon.dylib" \
  "-executable=$stage_dir/Contents/PlugIns/tls/libqsecuretransportbackend.dylib" \
  "${deploy_args[@]}"
if grep -q '^NAVIDOG_WITH_JDBC:BOOL=ON$' "$build_dir/CMakeCache.txt"; then
  mkdir -p "$jdbc_bundle"
  ditto "$build_dir/jdbc/navidog-jdbc-bridge.jar" "$jdbc_bundle/navidog-jdbc-bridge.jar"
fi
mkdir -p "$stage_dir/Contents/Resources/translations"
ditto "$("$qtpaths_executable" --query QT_INSTALL_TRANSLATIONS)/qtbase_zh_CN.qm" "$stage_dir/Contents/Resources/translations/qtbase_zh_CN.qm"
version="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$stage_dir/Contents/Info.plist")"
if [[ -f "docs/release-$version.md" ]]; then
  ditto "docs/release-$version.md" "$stage_dir/Contents/Resources/release-notes.md"
fi
mkdir -p "$stage_dir/Contents/Resources/licenses"
ditto LICENSE "$stage_dir/Contents/Resources/licenses/Navidog-LICENSE"
ditto NOTICE "$stage_dir/Contents/Resources/licenses/Navidog-NOTICE"
find "$build_dir" -path '*/odpi-*/LICENSE*' -type f -maxdepth 6 -exec cp {} "$stage_dir/Contents/Resources/licenses/" \;
mysql_client_root="$(sed -n 's/^NAVIDOG_MYSQL_CLIENT_ROOT:PATH=//p' "$build_dir/CMakeCache.txt")"
if [[ -n "$mysql_client_root" && -f "$mysql_client_root/LICENSE" ]]; then
  mkdir -p "$stage_dir/Contents/Resources/licenses/mysql-client"
  ditto "$mysql_client_root/LICENSE" "$stage_dir/Contents/Resources/licenses/mysql-client/LICENSE"
  for notice in INFO_BIN INFO_SRC; do
    if [[ -f "$mysql_client_root/$notice" ]]; then ditto "$mysql_client_root/$notice" "$stage_dir/Contents/Resources/licenses/mysql-client/$notice"; fi
  done
fi
ob_root="$(sed -n 's/^NAVIDOG_OCEANBASE_CLIENT_ROOT:PATH=//p' "$build_dir/CMakeCache.txt")"
if [[ -f "$obclient" && -n "$ob_root" ]]; then
  mkdir -p "$stage_dir/Contents/Resources/licenses/oceanbase-client"
  for license_item in COPYING.LIB source-url.txt; do
    if [[ -f "$ob_root/$license_item" ]]; then
      ditto "$ob_root/$license_item" "$stage_dir/Contents/Resources/licenses/oceanbase-client/$license_item"
    fi
  done
fi
codesign --force --deep --sign - "$stage_dir"
codesign --verify --deep --strict "$stage_dir"
if [[ "${NAVIDOG_PACKAGE_SMOKE_TEST:-1}" == 1 ]]; then
  "$stage_dir/Contents/MacOS/navidog" --version
  if [[ -x "$build_dir/jdbc/runtime/bin/java" ]]; then
    "$build_dir/jdbc/runtime/bin/java" -version
  fi
fi

case "$(lipo -archs "$stage_dir/Contents/MacOS/navidog")" in
  arm64) edition=Arm ;;
  x86_64) edition=Intel ;;
  *) edition=Universal ;;
esac
case "$edition" in
  Arm) python3 scripts/package-driver-components.py "$build_dir" "$package_dir" macos-arm64 ;;
  Intel) python3 scripts/package-driver-components.py "$build_dir" "$package_dir" macos-x86_64 ;;
esac
artifact="$package_dir/Navidog-$version-macOS-$edition"
ditto -c -k --keepParent "$stage_dir" "$artifact-Portable.zip"
sign_args=()
if [[ -n "${NAVIDOG_PKG_SIGN_IDENTITY:-}" ]]; then
  sign_args=(--sign "$NAVIDOG_PKG_SIGN_IDENTITY")
fi
productbuild ${sign_args[@]+"${sign_args[@]}"} --component "$stage_dir" /Applications "$artifact-Setup.pkg"
payload="$(pkgutil --payload-files "$artifact-Setup.pkg")"
[[ "$payload" == *Navidog.app/Contents/MacOS/navidog* ]]
