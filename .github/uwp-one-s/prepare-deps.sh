#!/usr/bin/env bash
# Import libraries and headers for the two DLLs the original package ships that no package manager has:
# SDL3.dll (the UWP build of SDL 3) and smb2.dll (libsmb2 6.2). Both are linked through import libraries
# made from the DLLs themselves, so the rebuilt exe binds to the very files it ships with.
#
#   prepare-deps.sh <unpacked original appx> <libsmb2 source zip> <report dir>
#
# Writes ../deps/sdl3-uwp (headers, libSDL3.dll.a, lib/pkgconfig/sdl3.pc) and ../deps/libsmb2-xemu, the
# path block/meson.build looks for. SDL3_HEADERS_REPO and SDL3_HEADERS_REF name the headers' source.
set -euo pipefail
orig=$(cd "$1" && pwd) # absolute: the gendef calls below run from other directories
smb2_zip=$2
report=$3
deps=$(cd .. && pwd)/deps
mkdir -p "$report" "$deps"

# gendef exits 0 when it cannot open the DLL and dlltool exits 0 on an empty def, so check the result.
import_lib_defines() { nm "$1" | grep -q " I __imp_$2\$" || { echo "$1 does not define $2" >&2; exit 1; }; }

ver=$(strings -n 8 "$orig/SDL3.dll" | sed -nE 's/^SDL-([0-9]+\.[0-9]+\.[0-9]+)-.*/\1/p' | head -1)
[ -n "$ver" ] || { echo "no SDL-x.y.z version string in $orig/SDL3.dll" >&2; exit 1; }
echo "SDL3.dll version $ver ($(strings -n 8 "$orig/SDL3.dll" | grep -m1 '^SDL-'))"

sdl=$deps/sdl3-uwp
rm -rf "$sdl"
mkdir -p "$sdl/include" "$sdl/lib/pkgconfig"
curl -sfL "https://github.com/$SDL3_HEADERS_REPO/archive/$SDL3_HEADERS_REF.tar.gz" |
  tar -xz -C "$sdl/include" --strip-components=2 --wildcards '*/include/SDL3'
hdr_ver=$(awk '/#define SDL_(MAJOR|MINOR|MICRO)_VERSION/ { v = v (v ? "." : "") $3 } END { print v }' "$sdl/include/SDL3/SDL_version.h")
if [ "$hdr_ver" != "$ver" ]; then
  echo "SDL3 headers are $hdr_ver, the DLL is $ver" >&2
  exit 1
fi
(cd "$sdl/lib" && gendef - "$orig/SDL3.dll" > SDL3.def && dlltool -d SDL3.def -D SDL3.dll -l libSDL3.dll.a)
import_lib_defines "$sdl/lib/libSDL3.dll.a" SDL_Init
echo "libSDL3.dll.a: $(grep -c '^SDL_' "$sdl/lib/SDL3.def") exports"
cat > "$sdl/lib/pkgconfig/sdl3.pc" <<EOF
prefix=$sdl
includedir=\${prefix}/include
libdir=\${prefix}/lib

Name: sdl3
Description: SDL $ver, the UWP SDL3.dll from the original package, headers from $SDL3_HEADERS_REPO@$SDL3_HEADERS_REF
Version: $ver
Cflags: -I\${includedir}
Libs: -L\${libdir} -lSDL3
EOF
printf 'SDL3 %s (headers %s@%s, import library from the shipped SDL3.dll)\n' "$ver" "$SDL3_HEADERS_REPO" "$SDL3_HEADERS_REF" > "$report/sdl3.txt"

smb2=$deps/libsmb2-xemu
rm -rf "$smb2"
mkdir -p "$smb2/include" "$smb2/lib"
unzip -q "$smb2_zip" 'libsmb2-*/include/smb2/*' -d "$smb2/tmp"
mv "$smb2"/tmp/libsmb2-*/include/smb2 "$smb2/include/smb2"
rm -rf "$smb2/tmp"
(cd "$smb2/lib" && gendef - "$orig/smb2.dll" > smb2.def && dlltool -d smb2.def -D smb2.dll -l libsmb2.dll.a)
import_lib_defines "$smb2/lib/libsmb2.dll.a" smb2_init_context
echo "libsmb2.dll.a: $(grep -c '^smb2_' "$smb2/lib/smb2.def") exports"
echo "libsmb2 headers: $(ls "$smb2/include/smb2" | tr '\n' ' ')"
