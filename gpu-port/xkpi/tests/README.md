# NMX host checks

These checks do not submit GPU work or establish graphics reliability.

- `python3 tests/ibprepare-host.py "$GPU_SRC/drm-kmod/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c"`
  compiles the actual patched CS helper with mapping/kmap fixtures; exercises canonical addresses,
  offsets, snapshot limits, bounds and error/skip paths. Uses xcrun clang on the build host.
- `python3 tests/patch-replay-host.py` stages vendor HEAD in a temporary Git fixture, applies the
  complete ordered patch series twice, checks refusal/preservation of unknown edits, and verifies
  stale-object removal and refusal to link incomplete builds. Set GPU_SRC to select a vendor checkout.

Build through rt.sh, all.sh and kext/build.sh. Runtime/driver failure or interruption leaves an
incomplete marker; rebuild that stage successfully before linking. Do not delete the marker just
because an older object exists. Patch replay refuses edits not represented by the patch series.
