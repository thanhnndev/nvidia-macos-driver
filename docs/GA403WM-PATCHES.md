# Branch `ga403wm`: upstream 1.14.7 + two patches for the ASUS ROG Zephyrus G14 (GA403WM)

Fork of [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver) kept by the
[ga403wm-hackintosh](https://github.com/thanhnndev/ga403wm-hackintosh) project. Same licence as upstream (PolyForm
Noncommercial 1.0.0). **No binaries are published here.** The branch is `v1.14.7` plus these changes (and this file), nothing else:

| Commit | What |
|---|---|
| `kexts/NVRM/nvrm_edpbl.cpp` + 9 lines in `os-nvkms-xnu.cpp` | eDP panels with **AUX-only brightness** (no PWM pin, DPCD `0x721` = 2, level in `0x722/3`): RM's `SET_BACKLIGHT_BRIGHTNESS` returns success and does nothing there, so the level is written over DPCD on a private kernel RM client |
| `kexts/NVRMEdpBL/` | the same code packaged as its own kext (NVRM is only buildable inside NullMoth's tree); `NVRMFB` asks that service first and falls back to NVRM, so a machine without the kext behaves as upstream |
| `kexts/NVRMFB/fb/nvrm-fb.cpp` | VRAM grant clear with non-temporal stores (`debug.nvrmfb_clear_mode`), plus the hook above |
| `kexts/NVRMEdpBL/NVRMEdpBL.cpp` | `KMOD_EXPLICIT_DECL`, without which `kmutil` cannot link the kext |

Boot-arg `nvrmedpbl=0|1|2`: 0 off, 1 AUX-only panels (default), 2 any panel with the AUX capability.

## Use

You still need the **upstream release** (its package or `install.sh`): it provides NVRM, NVAccel, the WindowServer plugin and
the OpenCore changes. This branch only replaces `NVRMFB` and adds `NVRMEdpBL.kext` on top. Build, dry run, install and
undo are in `scripts/` of the ga403wm-hackintosh repository
(`macos-build-nullmoth-local.sh`, `macos-aux-kc-dryrun.sh`, `macos-edpbl-install.sh [--undo]`; all root steps through
`sudo`/osascript, hash-checked, with backups under `/Library/NullMoth/edpbl-patch`) and in its
`docs/OLED-BRIGHTNESS-DPCD.md`. Building needs the open-gpu-kernel-modules 610.57.04 headers.

Only checked on one machine (RTX 5060 Laptop + 2880x1800 OLED, macOS 15.8.1). A panel that is not AUX-only is left to RM.
Keep upstream's rules: macOS 15 only, back up your EFI, start once without the NVIDIA driver if something goes wrong.
