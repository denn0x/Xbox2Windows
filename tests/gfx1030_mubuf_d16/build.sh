#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

cd "$(dirname "$0")"
llvm_mc=${LLVM_MC_15:-llvm-mc-15}
lld=${LD_LLD_15:-ld.lld-15}
objdump=${LLVM_OBJDUMP_15:-llvm-objdump-15}

target=${1:-gfx1030}
case "$target" in
    gfx1030) source=d16.s; mnemonic=buffer_store_format_d16 ;;
    gfx1100) source=d16-gfx1100.s; mnemonic=buffer_store_d16_format ;;
    *) echo "Usage: sh build.sh [gfx1030|gfx1100]" >&2; exit 2 ;;
esac

output="../../out/native-mubuf-d16/$target"
mkdir -p "$output"
"$llvm_mc" -triple=amdgcn-amd-amdhsa -mcpu="$target" -filetype=obj "$source" -o "$output/xyz.o"
"$llvm_mc" -triple=amdgcn-amd-amdhsa -mcpu="$target" -filetype=obj --defsym=CONTROL_XYZW=1 "$source" -o "$output/xyzw.o"
"$lld" -shared "$output/xyz.o" -o "$output/xyz.hsaco"
"$lld" -shared "$output/xyzw.o" -o "$output/xyzw.hsaco"

for shape in xyz xyzw; do
    "$objdump" -d --mcpu="$target" "$output/$shape.hsaco" > "$output/$shape.disasm"
    grep -E "${mnemonic}_${shape}[[:space:]]" "$output/$shape.disasm"
done
