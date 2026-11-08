#!/bin/bash
# nmxagdc_build.sh <src kexts/NMXAGDC> <out>: the AGDC service for the 890M (NMXDeviceControl), as NMXAGDC.kext.
set -uo pipefail
SRC=$1; OUT=$2; mkdir -p "$OUT/NMXAGDC.kext/Contents/MacOS"
SDK="$(xcrun --show-sdk-path)"; KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
xcrun clang++ -arch x86_64 -fapple-kext -mkernel -nostdinc -I"$KHDR" -I"$SRC" -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
  -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -O2 \
  -c "$SRC/nmx-agdc.cpp" -o "$OUT/nmx-agdc.o" 2>"$OUT/nmxagdc.err" || { grep error "$OUT/nmxagdc.err" | head -20; exit 1; }
xcrun clang++ -arch x86_64 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext "$OUT/nmx-agdc.o" -o "$OUT/NMXAGDC.kext/Contents/MacOS/NMXAGDC" 2>"$OUT/nmxagdcld.err" || { head "$OUT/nmxagdcld.err"; exit 1; }
cp "$SRC/Info.plist" "$OUT/NMXAGDC.kext/Contents/Info.plist"
echo "NMXAGDC $(md5 -q "$OUT/NMXAGDC.kext/Contents/MacOS/NMXAGDC") $(stat -f %z "$OUT/NMXAGDC.kext/Contents/MacOS/NMXAGDC") B"
