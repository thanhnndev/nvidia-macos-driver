#!/bin/bash
# Build the standalone AGDC companion from this checkout, without a staged home tree.
set -euo pipefail
SRC=${1:?NVRMAGDC source directory}; OUT=${2:?output directory}
SDK=$(xcrun --show-sdk-path); KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
mkdir -p "$OUT/NVRMAGDC.kext/Contents/MacOS"
xcrun clang++ -arch x86_64 -fapple-kext -mkernel -nostdinc -I"$KHDR" -I"$SRC" \
 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions \
 -fno-builtin -fno-common -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override \
 -Wno-unused-function -O2 -c "$SRC/nvrm-agdc.cpp" -o "$OUT/nvrm-agdc.o" 2>"$OUT/agdc.err"
xcrun clang++ -arch x86_64 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
 "$OUT/nvrm-agdc.o" -o "$OUT/NVRMAGDC.kext/Contents/MacOS/NVRMAGDC" 2>"$OUT/agdc-link.err"
cp "$SRC/Info.plist" "$OUT/NVRMAGDC.kext/Contents/Info.plist"
shasum -a 256 "$OUT/NVRMAGDC.kext/Contents/MacOS/NVRMAGDC"
