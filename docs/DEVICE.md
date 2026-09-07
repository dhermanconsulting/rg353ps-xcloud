# Device profile

Everything below was measured over SSH on a stock RG353P. SSH is enabled out
of the box and `root` is the only user; `scripts/setup-device.sh` records the
address and credentials for the rest of the tooling.

Numbers are from one unit on one network. Treat the hardware facts as fixed
and the network figures as an illustration of the shape, not a target.

## Identity

| Property | Value |
|---|---|
| Model | **Anbernic RG353PS** |
| `/proc/device-tree/model` | `Rockchip RK3566 RG353P` (the firmware does not distinguish the PS) |
| `compatible` | `rockchip,rk3566-evb2-lp4x-v11` (generic Rockchip EVB DT) |
| OS version | `31-dev 2023/04/01 05:33` (rebranded Batocera 31) |
| Base | Buildroot 2021.05-git |
| Kernel | Linux 4.19.172 aarch64, Rockchip BSP, built Apr 2023 |
| CPU | 4x Cortex-A55 @ 1800 MHz |
| GPU | Mali-G52, proprietary `/usr/lib/libmali.so` (45 MB blob) |
| RAM | **MemTotal 986384 kB (~963 MB)** |
| Panel | 640x480 @ 60 Hz, the only mode on the internal connector |

### The device tree says RG353P; the hardware is an RG353PS

The **RG353PS** is the 1 GB, no-eMMC variant, and the stock firmware's device
tree model string does not distinguish it from the RG353P. Two independent
measurements agree with the hardware:

- The runtime device tree memory node totals ~1 GiB. U-Boot patches this node
  from detected DRAM, so this is physical, not a cap.
- `/proc/device-tree/sdhci@fe310000` is **`status=disabled`** and
  `/sys/class/mmc_host` lists only `mmc1` (SD `SK32G`, 32 GB, the Linux install),
  `mmc2` (SD `EE4S5`, 256 GB, ROMs) and `mmc3` (SDIO, wifi). There is no
  `mmcblk0` and no `/dev/block/by-name`.

**Consequence: there is no Android partition on this unit, so the Android Game
Pass route does not exist here** — which is what makes a native client the only
route, and this project necessary.

## Storage and filesystem

- `/` is a read-only squashfs (`/dev/loop0` on `/overlay/base`) under an
  overlayfs whose upper layer is a 256 MB tmpfs. Changes to `/` are lost on
  reboot unless committed with `anbernic-save-overlay`.
- `/userdata` is real writable ext4, 5.9 GB, 4.9 GB free.
- `/userdata/roms` is the 256 GB exFAT card, 226 GB free.
- `/boot` is partition 1 of the 32 GB card, 709 MB free, holds
  `extlinux/extlinux.conf`, `linux`, `initrd.gz`, `rk3566.dtb`, `uboot.img`.

## Userspace

Present: glibc 2.32, Python 3.9.2, curl 7.75.0, OpenSSL 1.1.1j, pacman 5.2.1,
SDL 2.0.22, ffmpeg 4.3-Kodi, **Moonlight Embedded 2.5.2**, **PortMaster 6.55**.

Absent: node, npm, gcc, make, cmake, git, X11, Wayland, any browser, GStreamer,
ffplay, ffprobe, `/dev/video*`.

SDL2's only real video backend is **KMSDRM**. EmulationStation draws straight to
DRM, so it holds DRM master while running.

## Media hardware

- `/dev/mpp_service` and `/dev/rga` exist. `librockchip_mpp.so.0`, `librga.so`.
- dmesg confirms the blocks probe: `mpp_service mpp-srv: probe start`,
  `mpp_vdpu2 fdea0400.vdpu: probing finish`, plus `mpp_rkvenc` and `mpp_vepu2`,
  each behind its own `rk_iommu`.
- VOP2 video port 1 has plane mask `0x15`, so overlay planes are available for a
  zero-copy NV12 present.
- ffmpeg was built `--enable-rkmpp --enable-libdrm` and exposes `h264_rkmpp`,
  `hevc_rkmpp`, `vp8_rkmpp`, `vp9_rkmpp`, `mpeg2_rkmpp` and the `drm` hwaccel.

## The decode result that matters

Test clip: `mandelbrot` source, H.264 High profile, CABAC on, no B-frames,
16.7 Mbit/s, 1280x720@60, 5 seconds. Deliberately hard to decode.

| Path | Result |
|---|---|
| Software, 4 threads, 720p60 | **2.54x realtime**, 7.24 s CPU per 5 s video (~1.45 cores, ~36% of the SoC) |
| Software, 1 thread, 720p60 | 0.74x realtime, cannot keep up |
| Software, 4 threads, 1080p60 | 1.27x realtime, 14.65 s CPU per 5 s (~73% of the SoC) |
| **Hardware `h264_rkmpp`** | **FAILS** |

### Why hardware decode fails - CMA hypothesis REFUTED

The failure log is:

```
mpp_rt: found drm allocator
mpp_log: got the /dev/mpp_service
[h264_rkmpp] Decoder noticed an info change (1280x720), format=0
mpp_device: mpp_device_send_request ioctl MPP_IOC_CFG_V1 failed ret -1 errno 12 Cannot allocate memory
mpp_device: mpp_device_send_reg ioctl VPU_IOC_SET_REG failed ret 12 errno 12
[h264_rkmpp] Received a errinfo frame.
```

`errno 12` is `ENOMEM`, and `/proc/meminfo` shows a 16 MB CMA pool with
`CmaFree: 0`, which looked like the obvious cause. **It is not.** Three
measurements kill that theory:

1. **A 320x240 clip fails identically.** Sixteen frames at that size is under
   2 MB. There was ~14 MB of reclaimable CMA. Size is not the constraint.
2. **`CmaAllocated` never moves.** Sampled at 5 Hz across a whole failing
   decode, it stayed pinned at 1944 kB. The decoder is not allocating from CMA
   at all.
3. **The relevant IOMMUs are enabled and bound.** `vdpu_mmu` (`fdea0800`),
   `rkvdec_mmu` (`fdf80800`) and `vop_mmu` (`fe043e00`) all report
   `status=okay` in the device tree and appear under
   `/sys/bus/platform/drivers/rk_iommu/`. With an IOMMU domain present,
   `rockchip_gem_alloc_buf()` takes the shmem path, not the CMA path, so MPP's
   buffers are scattered pages mapped through the IOMMU and the CMA pool size is
   irrelevant to them.

### The actual root cause: userspace/kernel MPP version skew

> **Superseded 2026-09-05 by [HARDWARE-DECODE.md](HARDWARE-DECODE.md).** This section has
> the right shape but the wrong mechanism, and its route 3 below is a dead end
> — do not spend time on it. The real cause is narrower and worse: the device's
> `librockchip_mpp.so.0` contains H.264 HALs for VDPU1, VDPU2 and RKVDEC**1**
> only, and has no `vdpu34x`/RKVDEC**2** family at all, which is the block
> RK3566 actually has (`mpp_rkvdec2 fdf80200.rkvdec`). It is not an ioctl
> generation skew and not a buffer-allocation problem, so inverting the buffer
> flow cannot help — the register block itself is generated by the wrong HAL.
> The only route is to build and ship a newer MPP. Everything below is kept
> because the measurements in it are still good.

`dmesg` during a failing decode is unambiguous:

```
mpp_dma_import_fd:195: dma_buf_get fd 259 failed
mpp_task_attach_fd:1343: can't import dma-buf 259
mpp_translate_reg_address:1399: reg[31741]: 0x070b0103 fd 259 failed
mpp_task_dump_mem_region:1575: --- dump mem region ---
mpp_rkvdec2 fdf80200.rkvdec: no memory region mapped
mpp_process_task:368: alloc_task failed.
```

Read `reg[31741]`. A register index of 31741 is nonsense. The userspace
`librockchip_mpp.so` is writing a register layout the `mpp_rkvdec2` kernel
driver does not expect, so the driver reads a bogus fd out of the wrong offset,
`dma_buf_get()` fails on it, and the task is rejected. `ENOMEM` is just what
`mpp_task_attach_fd()` substitutes for any dma-buf import failure.

The userspace library confirms the suspicion itself: it reports
`mpp version: unknown mpp version for missing VCS info`.

Earlier in the same log the hardware actually faults:

```
mpp_rkvdec2 fdf80200.rkvdec: fault addr 0x00000000 status 4b
mpp_task_timeout_work:326: task processing time out!
mpp_rkvdec2 fdf80200.rkvdec: resetting...
```

and one attempt produced a kernel backtrace in `mpp_translate_reg_address+0xd0`
from `mpp_dev_ioctl`, tainting the kernel. **Stop exercising `h264_rkmpp` on
this image.** It faults the VPU and can wedge the decoder until reset.

Note the block in use is `mpp_rkvdec2` at `fdf80200.rkvdec`, not the Hantro
`vdpu2`.

**This is a library/driver mismatch, not a tunable.** The routes to hardware
decode, in order of sanity:

1. Do not bother. Software decode at 720p60 is 2.54x realtime, ~36% of the SoC.
2. Cross-compile a `librockchip_mpp.so` from the `rockchip-linux/mpp` revision
   that matches this 4.19 BSP `mpp_service` ABI and ship it alongside the client
   via `LD_LIBRARY_PATH`, never overwriting the system copy.
3. Invert the buffer flow the way moonlight's `rk.c` does: allocate DRM dumb
   buffers yourself and commit their fds into MPP with `MPP_BUFFER_TYPE_DRM`.
   This may or may not survive the same register-layout bug.

**This also explains the community reports** of Moonlight hanging at the
Anbernic logo on stock firmware. Moonlight's `rk` platform drives MPP. My
640x480 test did not fail fast, it hung and had to be killed, which is exactly
that symptom. **Use moonlight's `sdl` platform, not `rk`.**

**Do not change the kernel command line.** There is no `linux,cma` node and no
`cma=` argument, so a `cma=` argument would take effect cleanly, but it would
fix nothing. The promising lead instead is Moonlight's inverted buffer flow:
allocate DRM dumb buffers yourself and push the fds into MPP with
`mpp_buffer_commit(grp, &info)` using `MPP_BUFFER_TYPE_DRM`, which takes MPP's
own allocator out of the path entirely.

**This does not block the project.** Software decode at 720p60 runs at 2.54x
realtime using about 36% of the SoC. Hardware decode is an optimisation.

Current boot line, `/boot/extlinux/extlinux.conf`, unmodified:

```
LABEL anbernic.linux
  LINUX /boot/linux
  FDTDIR /boot/dtb
  APPEND initrd=/boot/initrd.gz label=ANBERNIC rootwait quiet loglevel=0 console=ttyUSB0,1500000 fbcon=rotate:0 video=HDMI-A-1:1280x720@60
```

## Input

`retrogame_joypad`, `Bus=0019 Vendor=484b Product=1101`, on `/dev/input/js0` and
`event3`, with real axes and buttons. Separate `adc-keys` and `gpio-keys`
keyboard devices carry some buttons, plus `rk805 pwrkey`.

## Network

- Realtek `8821cs` SDIO on `wlan0`, VHT enabled, associated to a 5 GHz
  network at RSSI -57 dBm.
- ping 1.1.1.1: **12 ms avg**, 0% loss.
- HTTPS download: **8.4 MB/s (~67 Mbit/s)**. xCloud wants roughly 20 Mbit/s.
- ping xbox.com: 83 ms.
- HTTPS to `login.microsoftonline.com` succeeds, so the MSAL device-code
  endpoint is reachable and TLS works with OpenSSL 1.1.1j.

## Security note

`/boot/anbernic-boot.conf` stores the wifi SSID and passphrase in **plaintext**
on the FAT boot partition, which is readable by anyone who puts the card in a
PC. Worth knowing; consider a guest network for the device.
