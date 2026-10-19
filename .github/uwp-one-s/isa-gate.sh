#!/usr/bin/env bash
# Counts the instructions in a PE executable that an AMD Jaguar (Xbox One, One S: GCC's btver2) cannot run:
# BMI2, FMA3 and AVX2. Exits 1 when any are found, so a build that would crash the console with
# STATUS_ILLEGAL_INSTRUCTION never gets packed. Each offender is listed with the function it sits in.
# Works with GNU objdump (MSYS2) and LLVM objdump (macOS): both print "<address>: <mnemonic> <operands>"
# with --no-show-raw-insn and "<address> <function>:" before each function.
#
#   isa-gate.sh <objdump> <exe>
set -euo pipefail
objdump=$1
exe=$2

"$objdump" -d --no-show-raw-insn "$exe" | awk '
  /^[0-9a-f]+ <.*>:$/ { fn = $2; next }
  $1 ~ /^[0-9a-f]+:$/ {
    m = $2
    kind = ""
    if (m ~ /^(shlx|shrx|sarx|rorx|pdep|pext|mulx|bzhi)[lq]?$/) { bmi2++; kind = "BMI2" }
    else if (m ~ /^vfn?m(add|sub)(add|sub)?(132|213|231)[a-z]*$/) { fma3++; kind = "FMA3" }
    else if (m ~ /^(vpbroadcast[a-z]*|vpermq|vpermd|vperm2i128|vinserti128|vextracti128|vpgather[a-z]*|vpmaskmov[a-z]*|vpsllv[a-z]*|vpsrlv[a-z]*|vpsravd)$/) { avx2++; kind = "AVX2" }
    if (kind != "" && listed++ < 40) printf "%s %s %s in %s\n", kind, $1, m, fn
    total++
  }
  END {
    printf "instructions %d\nBMI2 %d\nFMA3 %d\nAVX2 %d\n", total, bmi2, fma3, avx2
    exit (bmi2 + fma3 + avx2) > 0
  }'
