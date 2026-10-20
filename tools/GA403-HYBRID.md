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
