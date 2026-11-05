#!/bin/bash
# Build NVRM.kext from kexts/NVRM against the RM and modeset objects build_nvrm_vendor.sh made.
#   build/build_nvrm.sh <out>      (OGKM defaults to ~/ogkm615; VERSION defaults to 2.7.1, the bundle version stamped
#                                   into Info.plist so the kext replaces the installed one under the same identity)
# Undefined symbols are resolved by the kernel at link-collection time, so the script compares them with the
# installed NVRM.kext and lists every one the installed kext does not also import.
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd); SRC=$REPO/kexts/NVRM; OUT=${1:?usage: build_nvrm.sh <out>}
OGKM=${OGKM:-$HOME/ogkm615}; VERSION=${VERSION:-2.7.1}; export OGKM
NV=$OGKM/src/nvidia; KMS=$OGKM/src/nvidia-modeset; RO=$NV/_out/Darwin_x86_64; KO=$KMS/_out/Darwin_x86_64
[ -f "$RO/compile_cmds.sh" ] && [ -f "$KO/compile_cmds.sh" ] || { echo "STOP: run build/build_nvrm_vendor.sh first"; exit 2; }
mkdir -p "$OUT/obj" "$OUT/NVRM.kext/Contents/MacOS"; OBJ=$OUT/obj; rm -f "$OBJ"/*.o

# vendor archives: every object the upstream makefiles compiled (their final ld -r step is Linux-only)
rm -f "$OUT/libnvkernel.a" "$OUT/libnvkms.a"
ar rcs "$OUT/libnvkernel.a" "$RO"/*.o || exit 3
ar rcs "$OUT/libnvkms.a" "$KO"/*.o || exit 3

SDK=$(xcrun --show-sdk-path); KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
RMINC=( -I"$NV/arch/nvalloc/unix/include" -I"$NV/arch/nvalloc/common/inc" -I"$NV/arch/nvalloc/common/inc/gsp"
  -I"$OGKM/src/common/sdk/nvidia/inc" -I"$OGKM/src/common/sdk/nvidia/inc/hw" -I"$OGKM/src/common/inc"
  -I"$OGKM/src/common/shared/inc" -I"$NV/inc" -I"$NV/inc/os" -I"$NV/inc/kernel" -I"$NV/kernel/inc" -I"$NV/interface"
  -I"$OGKM/src/common/nvlink/interface" -I"$OGKM/src/common/inc/swref" -I"$OGKM/src/common/inc/swref/published"
  -I"$NV/inc/libraries" -I"$NV/src/libraries" -I"$NV/generated" -I"$NV/src/mm/uvm/interface"
  -I"$OGKM/src/common/uproc/os/libos-v2.0.0/include" -I"$OGKM/src/common/uproc/os/common/include"
  -I"$OGKM/src/common/inc/displayport" -I"$OGKM/src/common/nvlink/inband/interface" )
KMSINC=( -I"$KMS/os-interface/include" -I"$KMS/kapi/interface" -I"$KMS/kapi/include" -I"$OGKM/src/common/unix/nvidia-push/interface"
  -I"$KMS/interface" -I"$KMS/include" -I"$OGKM/src/common/unix/common/inc" -I"$OGKM/src/common/modeset"
  -I"$OGKM/src/common/unix/common/utils/interface" )
RMDEFS=(); while read -r d; do RMDEFS+=("$d"); done < <(head -1 "$RO/compile_cmds.sh" | tr ' ' '\n' | grep -E '^-D' | sed -e 's/"//g' | grep -v -E '^-D(NVRM|_LANGUAGE_C)$')
CXXFLAGS=( -arch x86_64 -fapple-kext -mkernel -nostdinc -I"$KHDR" -I"$SRC" "${RMINC[@]}" "${KMSINC[@]}"
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT "${RMDEFS[@]}"
  -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -O2 -ffile-prefix-map=$HOME=/build )
fail=0
for f in "$SRC"/*.cpp; do
  o=$OBJ/$(basename "${f%.cpp}").o
  xcrun clang++ "${CXXFLAGS[@]}" -c "$f" -o "$o" 2>"$o.err" || { echo "FAILED $(basename "$f")"; grep error "$o.err" | head -5; fail=1; }
done
# RM-internal C: RM's own osapi.c command line
python3 "$SRC/rmcc.py" "$SRC/nvrm_pageoff.c" "$OBJ/nvrm_pageoff.o" || { echo "FAILED nvrm_pageoff.c"; fail=1; }
# modeset-internal C: nvkms-kapi.c's command line, run from the modeset tree
python3 - "$KO/compile_cmds.sh" "$SRC/nvrm_kapi_fields.c" "$OBJ/nvrm_kapi_fields.o" "$KMS" <<'EOF' || { echo "FAILED nvrm_kapi_fields.c"; fail=1; }
import shlex, subprocess, sys
cmds, src, out, cwd = sys.argv[1:5]
line = next(l for l in open(cmds) if ' -c kapi/src/nvkms-kapi.c ' in l)
t = shlex.split(line.split(' && ')[0])
t[t.index('-c') + 1] = src; t[t.index('-o') + 1] = out
for f in ('-MF', '-MT'):
    if f in t: t[t.index(f) + 1] = out + '.d'
sys.exit(subprocess.run(t, cwd=cwd).returncode)
EOF
# the modeset ID string: upstream generates g_nvid_string.c only as part of its Linux-only final link
printf '%s\n' 'const char NV_KMS_ID[] = "nvidia id: NVIDIA UNIX Open Kernel Mode Setting Driver for x86_64 615.78.08 Release Build (local build)";' \
  'const char *const pNV_KMS_ID = NV_KMS_ID + 11;' > "$OBJ/g_nvkms_id.c"
xcrun clang -arch x86_64 -mkernel -c "$OBJ/g_nvkms_id.c" -o "$OBJ/g_nvkms_id.o" || fail=1
[ $fail = 0 ] || exit 4

B=$OUT/NVRM.kext/Contents/MacOS/NVRM
xcrun clang++ -arch x86_64 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  "$OBJ"/*.o "$OUT/libnvkms.a" "$OUT/libnvkernel.a" -o "$B" 2>"$OUT/ld.err" || { head -30 "$OUT/ld.err"; exit 5; }
sed -e "s|<string>0.1</string>|<string>$VERSION</string>|g" "$SRC/Info.plist" > "$OUT/NVRM.kext/Contents/Info.plist"
plutil -lint "$OUT/NVRM.kext/Contents/Info.plist" >/dev/null || exit 6

LIVE=/Library/Extensions/NVRM.kext/Contents/MacOS/NVRM
if [ -f "$LIVE" ]; then
  comm -13 <(nm -u "$LIVE" | sort -u) <(nm -u "$B" | sort -u) > "$OUT/new-undefined.txt"
  echo "imports not in the installed kext: $(wc -l < "$OUT/new-undefined.txt" | tr -d ' ') (see $OUT/new-undefined.txt)"
  echo "defined symbols: built $(nm -U "$B" | wc -l | tr -d ' ')  installed $(nm -U "$LIVE" | wc -l | tr -d ' ')"
fi
echo "NVRM $(shasum -a 256 "$B" | cut -c1-16) $(stat -f %z "$B") B -> $OUT/NVRM.kext"
