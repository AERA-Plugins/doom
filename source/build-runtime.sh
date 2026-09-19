#!/bin/bash
set -euo pipefail

pured_source=${PUREDOOM_SOURCE:?Set PUREDOOM_SOURCE to the pinned PureDOOM checkout}
sysroot=${AERA_SYSROOT:-/tmp/aera-webkit-sysroot}
cc=${AERA_CC:?Set AERA_CC to an ARM64 musl compiler wrapper}
strip=${AERA_STRIP:?Set AERA_STRIP to an LLVM strip binary}
output=${1:?Pass the staged runtime output directory}
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
freedoom_archive=${FREEDOOM_ARCHIVE:-/tmp/freedoom-0.13.0.zip}

readonly pured_commit=355cfbd16fac119718879239336ee2ea408886bd
readonly freedoom_sha256=3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59

test "$(git -C "$pured_source" rev-parse HEAD)" = "$pured_commit"
if [ ! -f "$freedoom_archive" ]; then
  curl -L --fail --show-error \
    https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip \
    -o "$freedoom_archive"
fi
printf '%s  %s\n' "$freedoom_sha256" "$freedoom_archive" | sha256sum -c -

build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
"$cc" -std=c11 -O3 -DNDEBUG -fno-strict-aliasing \
  -ffunction-sections -fdata-sections -fstack-protector-strong \
  -I"$pured_source" "$script_dir/aera-doom.c" \
  -Wl,--gc-sections -Wl,-z,relro,-z,now -pthread -lm \
  -o "$build/aera-doom"
"$strip" --strip-unneeded "$build/aera-doom"

rm -rf "$output"
mkdir -p "$output/lib" "$output/usr/bin" \
  "$output/usr/share/aera-doom/licenses/puredoom" \
  "$output/usr/share/aera-doom/licenses/freedoom"
cp -a "$sysroot/lib/ld-musl-aarch64.so.1" \
  "$sysroot/lib/libc.musl-aarch64.so.1" "$output/lib/"
cp "$build/aera-doom" "$output/usr/bin/"
unzip -p "$freedoom_archive" freedoom-0.13.0/freedoom1.wad \
  > "$output/usr/share/aera-doom/doom.wad"
unzip -p "$freedoom_archive" freedoom-0.13.0/COPYING.txt \
  > "$output/usr/share/aera-doom/licenses/freedoom/COPYING.txt"
cp "$pured_source/LICENSE" \
  "$output/usr/share/aera-doom/licenses/puredoom/LICENSE"

test -x "$output/usr/bin/aera-doom"
test -s "$output/usr/share/aera-doom/doom.wad"
