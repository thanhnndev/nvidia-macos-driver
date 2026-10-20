# GA403 hybrid work profile (upstream 2.4.2)

Based on upstream v2.4.2 (`272ccd9`), with the retained AMD owned-display gate
and a per-process backend selection policy. The plugin supports one Vulkan
vendor per process. WindowServer always selects NVIDIA; normal applications
prefer NVIDIA when NVAccel is registered. Existing headless/armed checks still
apply: selecting NVIDIA does not bypass display readiness.

Dedicated probes named `nvmtltest`, `mtlprobe`, or `nvmtlrender` can choose the
AMD backend with `NVMTL_TEST_GPU=amd`, or NVIDIA with `NVMTL_TEST_GPU=nvidia`.
Other processes, including WindowServer, ignore this setting. This avoids
enumeration order binding WindowServer to AMD while iGPU development proceeds.

The machine installer preserves patched AMD libdrm/RADV, leaves NMXGPU loaded,
and sets `NMXDisplayPipeEnabled=false` in the NMXAccel personality to pause the
experimental AMD WindowServer pipe. This is not isolation from kernel faults.
The older NVIDIA-eDP brightness companion is parked: its NVRM 0.1 dependency
and private ABI have not been rebased, and hybrid OLED belongs to AMD.

Validate the policy with:

```sh
cc -Wall -Wextra -Werror tools/test_hybrid_policy.c -o /tmp/nvmtl-hybrid-policy
/tmp/nvmtl-hybrid-policy
```

After reboot, verify NVIDIA owns HDMI and WindowServer uses NVIDIA before
claiming the work path is operational. Separately run a forced AMD probe and
verify its GPU name and actual rendered pixels. A package install alone proves
neither display initialization nor desktop stability.

## Foreign AMD OLED presentation (Step53)

The first hybrid boot ran HDMI successfully, but OLED retained the console:
CoreDisplay repeatedly reported no display pipe for NMXGPUFB. NMX's direct AMD
pipe opt-out must be paired with a foreign NVIDIA presentation pipe. Identity,
real EDID and 1440x900 HiDPI at120 were registered even while OLED was frozen.

Opt-in NVAccel personality `NMXForeignDisplayEnabled=true` on the exact reviewed
25G241 kernel watches only NMXGPUFB marked `NMXNvidiaDisplayBridge=true`, head4.
The NMXGPU AMD personality must separately set `NMXNvidiaDisplayBridge=true`;
its framebuffer joins the same IOFBDependentID as upstream NVRMFB/NMIntelFB.
Default profiles retain their prior head0/Intel behavior. NMXAccel's direct
AMD pipe remains off; AMD application Metal is retained.

A deferred callback holds an accelerator reference, waits at most60s for the
NVIDIA primary pipe and an opened/ready AMD scanout, validates CPU kva, zero
NVIDIA physical/handle fields, native2880x1800 geometry/pitch/allocation bounds,
then enables the existing head4 registration path. Family registration checks
exact framebuffer ownership. Existing foreign copy code skips NVIDIA VRAM
allocation and page flips: NVIDIA IOSurfaces are CPU-copied into DCN35 scanout.
`NVAccelForeignDisplayArmed` and `debug.nvaccel_iop_ok_h4` must confirm runtime
binding/presentation after reboot. No runtime success is claimed by a build.
HDR correctness is still separate from this SDR presentation/identity fix.
