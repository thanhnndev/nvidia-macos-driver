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

## AGDC association fix (Step54)

Step53 created/armed the real OLED kernel pipe (heads_piped0x11), but it received
zero frames and CoreDisplay still could not find it. A fresh entitled helper's
IOPresentmentGetMap contained only HDMI. Stock NVRMAGDC scans only same-PCI
NVRMNVDAFramebuffer services and reports one contiguous head; it cannot advertise
our physical foreign head4. This is a separate requirement from pipe creation.

The scoped `NMXForeignDisplayEnabled=true` NVDAGDCLoader personality now appends
exactly one marked foreign NMXGPUFB at the compact AGDC index after native heads.
It must name this NVIDIA rendering PCI registry ID, matching dependent group and
head4, and have valid actual K5 timing. NVAccel declares rendering ownership at
publication before open; once registered it also publishes the actual pipe index,
which must match. Ambiguous foreign matches are refused. Real timing (including
sync polarity) comes from DRM, not a synthesized EDID. The hybrid map is rescanned
on each0x980 reply to avoid startup's one-second cache race. A table lock protects
retained entries during rescan/reply/K5 reads. Default profiles keep old behavior.

Before installing, a private helper-only interposer changed its own0x980 reply
from one entry to two: actual IOPresentment produced HDMI index0 and AMD OLED
index1 on the same nonzero NVAccel registry ID. No WindowServer injection or
kernel reply modification was used. This proves the association format and
cross-PCI mapping, not live presentation. New kernel runtime remains to be checked:
AGDC fbmap/maxfb2, fresh IOPresentment map2, iop_ok_h4 increasing, OLED desktop.

## IOPresentment client declaration (Step55)

Step54 runtime confirms both actual map entries and real timing queries, but WS
still receives AGDC information error0x10000003 immediately after AGDC rejects
0x2001 (4-byte client declaration). The earlier Strix AGDC already handled this
passive declaration; the NVIDIA companion omitted it. The scoped hybrid profile
now accepts exactly4 input bytes/no output, records the declaration and returns
success. It does not acknowledge GPU execution, a modeset or HDR capability.
Malformed sizes/pointers/output are rejected; unaligned input is handled safely.
Read-only nvrmfb_agdc_client_set/value counters expose the declaration handling.
Only NVRMAGDC needs replacing; existing association/timing/foreign-copy code stays.

The malformed-payload boundary checks pass and the kext links into the full
AuxKC. A separate IOPresentment construction helper reaches NVAccel's exclusive
display-pipe-client guard because WS owns it, and cannot fully initialize beside
WS. No control ownership was stolen or WS restarted to bypass that guard. Next
boot must verify client-set acknowledged, no AGDC information error, increasing
head4 frame counter and visible OLED; no runtime success claimed yet.
