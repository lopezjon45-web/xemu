#!/usr/bin/env bash
# Counts the instructions in a PE executable that an AMD Jaguar (Xbox One, One S: GCC's btver2) cannot run:
# BMI2, FMA3 and AVX2. Exits 1 when any are found, so a build that would crash the console with
# STATUS_ILLEGAL_INSTRUCTION never gets packed. Each offender is listed with the function it sits in.
# Works with GNU objdump (MSYS2) and LLVM objdump (macOS): both print "<address>: <mnemonic> <operands>"
# with --no-show-raw-insn and "<address> <function>:" before each function.
#
#   isa-gate.sh <objdump> <exe> [symbol map]
#
# The symbol map is the package's xemu.sym ("<rva> <name>" per line, sorted), for a stripped exe whose
# disassembly has no function labels.
#
# One exception: Rust's libm (linked through compiler-builtins) keeps fma_with_fma and fmaf_with_fma, each
# one FMA3 instruction, and calls them only after a cpuid check finds FMA
# (compiler-builtins, libm/src/math/arch/x86/fma.rs). They are counted and named but do not fail the gate.
set -euo pipefail
objdump=$1
exe=$2
map=${3:-/dev/null}
base=0
if [ "$map" != /dev/null ]; then
  base=$("$objdump" -p "$exe" | sed -nE 's/^ImageBase[[:space:]:]*(0x)?([0-9a-fA-F]+).*/\2/p' | head -1)
  [ -n "$base" ] || { echo "no ImageBase in $exe" >&2; exit 2; }
fi

"$objdump" -d --no-show-raw-insn "$exe" | awk -v base="$base" -v map="$map" '
  function hex(s,   i, c, v) {
    v = 0; s = tolower(s)
    for (i = 1; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)); if (c == 0) break; v = v * 16 + c - 1 }
    return v
  }
  function mapped(va,   rva, lo, hi, mid) {
    rva = va - imagebase
    if (n == 0 || rva < off[0]) return "?"
    lo = 0; hi = n - 1
    while (lo < hi) { mid = int((lo + hi + 1) / 2); if (off[mid] <= rva) lo = mid; else hi = mid - 1 }
    return nm[lo]
  }
  BEGIN {
    imagebase = hex(base)
    while ((getline line < map) > 0) { split(line, f, " "); off[n] = hex(f[1]); nm[n] = f[2]; n++ }
    allowed = "^_ZN4libm4math4arch3x863fma1[23]fmaf?_with_fma"
  }
  /^[0-9a-f]+ <.*>:$/ { fn = $2; gsub(/[<>:]/, "", fn); next }
  $1 ~ /^[0-9a-f]+:$/ {
    m = $2
    kind = ""
    if (m ~ /^(shlx|shrx|sarx|rorx|pdep|pext|mulx|bzhi)[lq]?$/) { bmi2++; kind = "BMI2" }
    else if (m ~ /^vfn?m(add|sub)(add|sub)?(132|213|231)[a-z]*$/) { fma3++; kind = "FMA3" }
    else if (m ~ /^(vpbroadcast[a-z]*|vpermq|vpermd|vperm2i128|vinserti128|vextracti128|vpgather[a-z]*|vpmaskmov[a-z]*|vpsllv[a-z]*|vpsrlv[a-z]*|vpsravd)$/) { avx2++; kind = "AVX2" }
    if (kind != "") {
      where = (n > 0) ? mapped(hex(substr($1, 1, length($1) - 1))) : fn
      note = ""
      if (where ~ allowed) { guarded++; note = " (runtime-dispatched, allowed)" }
      if (listed++ < 40) printf "%s %s %s in %s%s\n", kind, $1, m, where, note
    }
    total++
  }
  END {
    printf "instructions %d\nBMI2 %d\nFMA3 %d\nAVX2 %d\nallowed %d\n", total, bmi2, fma3, avx2, guarded
    exit (bmi2 + fma3 + avx2 - guarded) > 0
  }'
