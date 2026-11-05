#!/bin/bash
# Build NVIDIA's RM core (src/nvidia) and modeset core (src/nvidia-modeset) for macOS from open-gpu-kernel-modules
# 615.78.08, the version the 2.7.1 NVRM.kext carries. Writes _out/Darwin_x86_64/{libnvkernel.a,libnvkms.a,compile_cmds.sh}
# in each tree, which build_nvrm.sh, rmcc.py and accel_build.sh read.
#   OGKM=<tree> build/build_nvrm_vendor.sh      (default ~/ogkm615; the kms display patches must already be applied)
# macOS clang defines neither __unix__ nor __linux__: NV_UNIX picks the Unix kernel paths of nvport, NV_BSD only
# satisfies the version headers (nvUnixVersion.h, nvBldVer.h). NV_LINUX is not set: the 2.7.1 kext has no cgroup symbols.
set -uo pipefail
OGKM=${OGKM:-$HOME/ogkm615}; J=${J:-8}; DEFS="-DNV_UNIX -DNV_BSD"
# The modeset makefile adds -mindirect-branch / -mfunction-return=thunk-extern, which import __x86_return_thunk: XNU
# exports none and the 2.7.1 kext imports none. Keep only the two flags the RM makefile also uses.
KMSARGS=( "CONDITIONAL_CFLAGS=-fcf-protection=branch -fno-jump-tables" )
ONLY=${ONLY:-nvidia nvidia-modeset}
for t in $ONLY; do
  D=$OGKM/src/$t; O=$D/_out/Darwin_x86_64; A=(); [ $t = nvidia-modeset ] && A=( "${KMSARGS[@]}" )
  rm -rf "$D/_out"
  ( cd "$D" && CFLAGS="$DEFS" nice make -k -j"$J" "${A[@]}" > "$OGKM/$t-build.log" 2>&1 ); rc=$?
  echo "$t: make rc=$rc  errors=$(grep -c 'error:' "$OGKM/$t-build.log")  objects=$(ls "$O"/*.o 2>/dev/null | wc -l | tr -d ' ')"
  # the compile commands (one per line, the cc command first) as rmcc.py / build-completion-archive.py read them
  mkdir -p "$O"
  ( cd "$D" && CFLAGS="$DEFS" make -n -B "${A[@]}" 2>/dev/null ) | grep -E ' -c ' | sed -E 's/^printf [^&]*&& //' > "$O/compile_cmds.sh"
  echo "$t: $(wc -l < "$O/compile_cmds.sh" | tr -d ' ') compile commands"
done
