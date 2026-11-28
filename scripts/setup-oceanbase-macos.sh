#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
revision=2618276f5dd11a512aab21518994fef21c197816
checksum=1c45c7520e0aa1a559c959d28a8c9cb571c57b6d16c2d4671d88ea4c93626a7a
source_url="https://codeload.github.com/oceanbase/obconnector-c/tar.gz/$revision"
work="$root/.cache/oceanbase-connector-$revision"
sdk="$root/.cache/oceanbase-sdk"
archive="$work/source.tar.gz"
mkdir -p "$work/source" "$sdk/include" "$sdk/lib"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --silent --show-error --retry 2 "$source_url" -o "$archive.download"
    mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | shasum -a 256 -c
tar -xzf "$archive" -C "$work/source" --strip-components=1
cmake -S "$work/source" -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWITH_SSL=OPENSSL -DWITH_CURL=OFF \
    -DWITH_UNIT_TESTS=OFF -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build "$work/build" --target libobclnt --parallel 6
ditto "$work/source/include" "$sdk/include"
cp "$work/build/include/mariadb_version.h" "$work/build/include/ma_config.h" "$sdk/include/"
cp -L "$work/build/libmariadb/libobclnt.dylib" "$sdk/lib/libobclnt.dylib"
cp "$work/source/COPYING.LIB" "$sdk/COPYING.LIB"
printf '%s\n' "$source_url" > "$sdk/source-url.txt"
printf 'OceanBase Connector/C SDK: %s\n' "$sdk"
