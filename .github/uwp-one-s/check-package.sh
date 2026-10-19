#!/usr/bin/env bash
# Independent check of a built package, meant for a machine other than the runner (macOS with Apple's
# llvm-objdump, or MSYS2): SHA-256, manifest identity, signer, the instruction gate on xemu.exe, and the same
# counts on every DLL that did not come from the original package. Exits 1 only when xemu.exe fails the gate;
# MSYS2 libraries may carry BMI2/FMA3/AVX2 behind runtime CPU checks, so their counts are reported, not judged.
#
#   check-package.sh <appx> [dlls.tsv from the build report]
set -euo pipefail
appx=$1
tsv=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

shasum -a 256 "$appx" 2>/dev/null || sha256sum "$appx"

# An appx is a zip whose entry names are percent-encoded (libstdc++-6.dll is stored as libstdc%2B%2B-6.dll).
python3 - "$appx" "$work" <<'EOF'
import sys, zipfile, urllib.parse, pathlib
appx, out = sys.argv[1], pathlib.Path(sys.argv[2])
with zipfile.ZipFile(appx) as z:
    for info in z.infolist():
        dest = out / urllib.parse.unquote(info.filename)
        if info.is_dir():
            dest.mkdir(parents=True, exist_ok=True)
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(z.read(info))
EOF

echo
echo "identity: $(grep -o '<Identity [^>]*>' "$work/AppxManifest.xml")"
if [ -f "$work/AppxSignature.p7x" ]; then
  # The file is a 4-byte "PKCX" marker followed by the PKCS#7 DER blob.
  signer=$(tail -c +5 "$work/AppxSignature.p7x" | openssl pkcs7 -inform DER -print_certs -noout | grep -m1 '^subject=' || true)
  echo "signature: AppxSignature.p7x present, ${signer:-no certificate found}"
else
  echo "signature: AppxSignature.p7x missing"
fi

echo
echo "## xemu.exe (functions resolved through the package's xemu.sym)"
gate_status=0
"$here/isa-gate.sh" objdump "$work/xemu.exe" "$work/xemu.sym" || gate_status=$?

echo
echo "## DLLs"
printf '%-32s %-9s %10s %6s %6s %6s %8s\n' dll origin instrs BMI2 FMA3 AVX2 allowed
for f in "$work"/*.dll; do
  d=$(basename "$f")
  origin=unknown
  if [ -n "$tsv" ]; then
    origin=$(awk -F'\t' -v d="$d" 'tolower($1) == tolower(d) { print $2 }' "$tsv")
    [ -n "$origin" ] || origin="not in dlls.tsv"
  fi
  if [ "$origin" = original ]; then
    printf '%-32s %-9s\n' "$d" "$origin"
  else
    counts=$("$here/isa-gate.sh" objdump "$f" | grep -E '^(instructions|BMI2|FMA3|AVX2|allowed) ' | awk '{ printf "%s ", $2 }' || true)
    # shellcheck disable=SC2086
    printf '%-32s %-9s %10s %6s %6s %6s %8s\n' "$d" "$origin" $counts
  fi
done

echo
if [ $gate_status -eq 0 ]; then echo "xemu.exe gate: PASS"; else echo "xemu.exe gate: FAIL"; fi
exit $gate_status
