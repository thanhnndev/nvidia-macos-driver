#!/bin/bash
# kext/build.sh <out dir>: links NMXGPU.kext from the compiled port (obj = AMD/DRM/TTM, obj-i915 = Intel i915, obj-xe = Intel xe, lkobj = LinuxKPI runtime and
# the port's LinuxKPI-side files, srcobj = FreeBSD-on-xnu layer, xnuobj = IOKit glue and the kext itself).
set -euo pipefail
X=${GPU_SRC:-$HOME/develop/projects/gpu-src}/xkpi
OUT=$1
cd "$X"
for failed in .nmx-driver-build-incomplete .nmx-runtime-build-incomplete logs/*.failed; do
  [ ! -e "$failed" ] || { echo "refusing stale-object link: $failed"; exit 1; }
done
./ccxx.sh xnu/nmx_kext.cpp > xlog_nmx_kext.txt 2>&1 || { grep -A3 error: xlog_nmx_kext.txt | head -20; exit 1; }
# xe carries its own build of the Intel display code (compiled for xe_device), which would collide with i915's at link.
# So xe is first folded into one object that exports nothing: every xe symbol becomes local, xe's calls bind to its
# own copies, and it registers itself through the linker sets (SYSINIT/module params) like the other drivers.
# ga403wm: AMD_ONLY=1 links the AMD/DRM/TTM driver alone (no i915/xe objects; xe needs the pristine Linux tree)
INTEL="obj-i915/*.o xe_all.o"
if [ -n "${AMD_ONLY:-}" ]; then
  INTEL=""
else
  : > xe.exp
  xcrun ld -r -arch x86_64 obj-xe/*.o -exported_symbols_list xe.exp -o xe_all.o
fi
K="$OUT/NMXGPU.kext/Contents"
mkdir -p "$K/MacOS"
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext \
  obj/*.o $INTEL lkobj/*.o srcobj/*.o $(ls xnuobj/*.o | grep -v nmx_npu_kext) -lkmodc++ -lkmod -lcc_kext -o "$K/MacOS/NMXGPU" 2> link.log || {
    grep -E "error|Undefined|duplicate" link.log | head -40; exit 1; }
cp kext/Info.plist "$K/Info.plist"
echo "NMXGPU $(shasum -a 256 "$K/MacOS/NMXGPU" | cut -c1-8) $(stat -f %z "$K/MacOS/NMXGPU") B"
