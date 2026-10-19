#!/usr/bin/env bash
# Builds the package folder for makeappx from the unpacked original appx and the rebuilt executable.
#
#   assemble-package.sh <unpacked original appx> <package dir> <qemu-system-i386w.exe> <version> <report dir>
#
# The exe is stripped and its symbol list regenerated (xemu.sym, read by the port's crash reporter). Every
# MinGW DLL is taken from this MSYS2 installation, found the way the port's package.sh finds them: the
# import tables, walked from the exe, for names that exist in $MINGW_PREFIX/bin. The files only the UWP build
# has (SDL3, Mesa, dxil, smb2, assets, disk image, settings) stay as they were. Footprint files of the old
# package (signature, block map, content types, code integrity catalog) are removed; makeappx and signtool
# make new ones. report/dlls.tsv lists every DLL with its origin.
set -euo pipefail
orig=$1
pkg=$2
exe=$3
version=$4
report=$5
bin=${MINGW_PREFIX:-/ucrt64}/bin
uwp_dlls=(SDL3.dll opengl32.dll libgallium_wgl.dll dxil.dll vulkan_dzn.dll z-1.dll smb2.dll)

rm -rf "$pkg"
cp -r "$orig" "$pkg"
rm -rf "$pkg/AppxSignature.p7x" "$pkg/AppxBlockMap.xml" "$pkg/[Content_Types].xml" "$pkg/AppxMetadata"
mkdir -p "$report"

is_uwp() { local d; for d in "${uwp_dlls[@]}"; do [ "${d,,}" = "${1,,}" ] && return 0; done; return 1; }

cp "$exe" "$pkg/xemu.exe"
base=$(objdump -p "$exe" | sed -n 's/^ImageBase\s*//p')
nm --defined-only "$exe" |
  awk -v base=$((16#$base)) '$2 ~ /^[tT]$/ { printf "%08x %s\n", strtonum("0x" $1) - base, substr($3, 1, 120) }' |
  sort -k1,1 > "$pkg/xemu.sym"
strip "$pkg/xemu.exe"

declare -A origin
for f in "$pkg"/*.dll; do
  d=$(basename "$f")
  if is_uwp "$d"; then origin[$d]=original; else origin[$d]=dropped; rm "$f"; fi
done

seen=" "
todo=("$pkg/xemu.exe")
while [ ${#todo[@]} -gt 0 ]; do
  f=${todo[0]}; todo=("${todo[@]:1}")
  for dll in $(objdump -p "$f" | sed -n 's/^\s*DLL Name: //p'); do
    lc=${dll,,}
    case "$seen" in *" $lc "*) continue;; esac
    seen="$seen$lc "
    is_uwp "$dll" && continue
    [ -f "$bin/$dll" ] || continue
    cp "$bin/$dll" "$pkg/$dll"
    todo+=("$pkg/$dll")
    if [ "${origin[$dll]:-}" = dropped ]; then origin[$dll]=rebuilt; else origin[$dll]=added; fi
  done
done

for d in "${uwp_dlls[@]}"; do
  [ -f "$pkg/$d" ] || { echo "$d missing from the original package" >&2; exit 1; }
done
for d in SDL3.dll smb2.dll; do
  grep -q "DLL Name: $d" <(objdump -p "$pkg/xemu.exe") || { echo "rebuilt xemu.exe does not import $d" >&2; exit 1; }
done

sed -i "s/ Version=\"[0-9.]*\" ProcessorArchitecture/ Version=\"$version\" ProcessorArchitecture/" "$pkg/AppxManifest.xml"
grep -q "Version=\"$version\"" "$pkg/AppxManifest.xml" || { echo "version not set in AppxManifest.xml" >&2; exit 1; }
grep -q 'Name="GamerDad.Xemu" Publisher="CN=GamerDad"' "$pkg/AppxManifest.xml" || { echo "package identity changed" >&2; exit 1; }

{
  printf 'dll\torigin\n'
  for d in $(printf '%s\n' "${!origin[@]}" | sort -f); do
    printf '%s\t%s\n' "$d" "${origin[$d]}"
  done
} > "$report/dlls.tsv"
cat "$report/dlls.tsv"
