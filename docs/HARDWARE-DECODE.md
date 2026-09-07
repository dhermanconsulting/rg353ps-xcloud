# Hardware video decode on the RG353: why it fails, and what is left

Investigated 2026-09-05. This supersedes the "userspace/kernel MPP version
skew" section of [DEVICE.md](DEVICE.md), which had the right
shape but the wrong mechanism, and kills the route it called promising.

**Summary: the device's `librockchip_mpp.so.0` has no H.264 decoder HAL for the
block this SoC actually has.** It is not a tunable, not an ioctl-version
mismatch, and not something a different buffer-allocation strategy can work
around — which kills the route DEVICE.md called promising.

Two routes remain. Shipping a newer MPP is the one with the real payoff and
costs days. Before that, there is an hour's experiment worth doing: the library
*does* carry a working HAL for the Hantro VDPU2 block that sits beside the
broken one, and that block is enabled and healthy on this device.

## What the hardware is

`dmesg`, boot:

```
mpp_service mpp-srv: probe start
mpp_vdpu2   fdea0400.vdpu:   probing finish
mpp_vepu2   fdee0000.vepu:   probe device
mpp_jpgdec  fded0000.jpegd:  probing finish
mpp_rkvdec2 fdf80200.rkvdec: probing start
mpp_rkvdec2 fdf80200.rkvdec: sram_start 0x00000000fdcc0000, rcb_iova 0x0000000010000000,
                             sram_size 45056, rcb_size=65536
mpp_rkvdec2 fdf80200.rkvdec: probing finish
mpp_service mpp-srv: probe success
```

The kernel side is healthy and modern. Two details matter:

- The decoder MPP reaches for H.264 here is **`mpp_rkvdec2`**, i.e. the
  VDPU34x-generation core. A Hantro `vdpu2` sits beside it and is also probed
  and healthy — see "The VDPU2 side-route" below, which is the one thing in
  this document that is a live lead rather than a closed door.
- It sets up a **row-cache buffer in SRAM** (`rcb_iova`, `rcb_size=65536`).
  Userspace is expected to tell the driver about RCB via
  `MPP_CMD_SET_RCB_INFO`. That command exists in this kernel — `0x203`, in the
  gap in `/proc/mpp_service/supports-cmd`, whose `SEND_BUTT` is `0x204`.

## What the userspace library is

`/usr/lib/librockchip_mpp.so.0`, 1229360 bytes, dated 2023-04-03, shipped with
the firmware. Every H.264 decoder HAL it contains, from `strings`:

```
vdpu1_h264d_{init,deinit,gen_regs,start,wait,reset,flush,control}
vdpu2_h264d_{init,deinit,gen_regs,start,wait,reset,flush,control}
rkv_h264d_{init,deinit,gen_regs,start,wait,reset,flush,control}
```

and its own assertion string naming the decoder clients it knows about:

```
vcodec_type & ((1 << VPU_CLIENT_RKVDEC) | (1 << VPU_CLIENT_VDPU1) | (1 << VPU_CLIENT_VDPU2))
```

Searching the whole library for the RKVDEC2 HAL family returns **nothing**:

```
$ strings -a /usr/lib/librockchip_mpp.so.0 | grep -icE 'vdpu34|vdpu38|rkvdec2'
0
```

`rkv_h264d_*` is `hal_h264d_rkv.c`, the **RKVDEC1** register layout (RK3288 /
RK3399 generation). RK3566 needs `hal_h264d_vdpu34x.c`. It is not in this
build.

The library does detect the SoC — `rk3566` is in its `mpp_get_soc_name` table
— so it identifies the chip correctly and then has no matching decoder HAL to
select. It also reports `mpp version: unknown mpp version for missing VCS
info`, i.e. it was built from a tarball with no git metadata, so the version
cannot be read off directly.

Corroborating the age: the library's device layer is the pre-`mpp_service`
generation. It contains `mpp_device_send_reg`, `mpp_device_send_extra_info`,
`mpp_device_patch_add`, `mpp_get_ioctl_version`, and the ioctl names
`VPU_IOC_SET_REG` and `MPP_IOC_CFG_V1`. It contains **no** `mpp_dev_ioctl`,
`MppDev` or `MppReqV1` symbols — the modern service client is simply absent.
It knows the *path* `/dev/mpp_service` and talks the old `MPP_IOC_CFG_V1`
protocol at it.

## Why that produces the observed failure

The failure, from DEVICE.md:

```
mpp_dma_import_fd:195: dma_buf_get fd 259 failed
mpp_task_attach_fd:1343: can't import dma-buf 259
mpp_translate_reg_address:1399: reg[31741]: 0x070b0103 fd 259 failed
mpp_rkvdec2 fdf80200.rkvdec: no memory region mapped
```

`reg[31741]` is the register index from the kernel's translation table, and
`0x070b0103` is what it found there. The kernel splits that word the way it
splits a register that is supposed to hold a buffer reference —
`fd = value & 0x3ff = 0x103 = 259`, offset in the top bits — and 259 is not a
dma-buf fd in this process, so the import fails and the task is rejected.
`ENOMEM` is just what `mpp_task_attach_fd()` substitutes for any dma-buf import
failure, which is why the CMA-exhaustion theory looked plausible for so long.

A register index of 31741 is not a plausible index into any real register set.
What produces it is userspace and kernel disagreeing about the shape of the
register block: the kernel walks its RKVDEC2 translation table over a register
array that userspace filled in a different layout, and reads offsets that mean
nothing. The earlier `fault addr 0x00000000 status 4b` and the driver's
`resetting...` are the same disagreement reaching the hardware.

So the mechanism is **not** an ioctl-generation skew (the ioctl is dispatched
and reaches register translation, so the kernel is accepting the request), and
**not** buffer allocation. It is that the register block itself is being
generated by the wrong HAL.

The last link in that chain — that it specifically selects `rkv_h264d` and
writes an RKVDEC1 layout — is inference from (a) that being the only H.264 HAL
it has that is not Hantro, (b) `VPU_CLIENT_RKVDEC` being in its client list,
and (c) the failure being a register-layout disagreement rather than anything
else. It could be confirmed in one run with `mpp_debug`/`hal_h264d_debug` set,
at the cost of faulting the VPU once. It does not change the conclusion either
way: no RKVDEC2 HAL is present, so no configuration of this library can drive
this block correctly.

## Consequences for the routes DEVICE.md listed

| Route | Verdict |
|---|---|
| 1. Don't bother; software decode is 2.54x realtime | Still true, still the status quo |
| 2. Ship a newer `librockchip_mpp.so` via `LD_LIBRARY_PATH` | The route with the best payoff, and days of work |
| 3. Invert the buffer flow, moonlight's `mpp_buffer_commit` / `MPP_BUFFER_TYPE_DRM` | **Cannot work — drop it** |
| 4. *(new)* Steer MPP onto the Hantro VDPU2 HAL it already has | Cheap to disprove; try this first |

Route 3 was the one described as "the promising lead". It is not. It changes
*where the buffers come from*; the failure is in *what is written to the
decoder's registers*, which happens identically regardless of who allocated the
buffer. Committing DRM dumb-buffer fds into MPP would still generate an
RKVDEC1 register block for an RKVDEC2 core. Anyone picking this up later should
not spend the two days it would take.

This also explains the community reports of Moonlight hanging at the Anbernic
logo on stock firmware, and it explains them better than "MPP is broken":
Moonlight's `rk` platform drives this same library, so it hits the same wall.
**Use Moonlight's `sdl` platform, not `rk`** — unchanged advice, firmer reason.

## The VDPU2 side-route

The library has **three** H.264 HALs, and only one of them is the broken
choice. `vdpu2_h264d_*` is the Hantro VDPU2 HAL, and the Hantro block on this
SoC is present, enabled and probed:

```
/proc/device-tree/vdpu@fdea0400   compatible = rockchip,vpu-decoder-v2  status = okay
dmesg: mpp_vdpu2 fdea0400.vdpu: probing finish
```

Unlike RKVDEC2, the Hantro register layout is *old* and has not moved, so a
2019-era userspace HAL and this kernel's `mpp_vdpu2` driver should agree about
it — which is exactly what the RKVDEC2 path fails to do. If MPP could be made
to pick the VDPU2 HAL for H.264, hardware decode might work with the library
already on the device.

Two unknowns, and they are real:

1. **Does RK3566's VDPU2 actually decode H.264?** Rockchip generally documents
   RK356x's VDPU2 as MPEG-1/2/4, H.263, VP8 and JPEG, with H.264 assigned to
   RKVDEC — the H.264 unit may simply not be in that block on this part.
   Having the HAL in the library proves nothing; that library also ships HALs
   for chips this is not.
2. **Can the selection be steered?** MPP picks the decoder client from a
   `vcodec_type` bitmask that comes from its built-in SoC table, keyed by the
   name it reads from `/proc/device-tree/compatible` (the library contains
   that path, and `open /proc/device-tree/compatible error.`). With
   `VPU_CLIENT_RKVDEC` set for rk3566 it will prefer the RKVDEC HAL. Getting
   VDPU2 chosen instead means making the library see a different SoC — a bind
   mount over `/proc/device-tree/compatible`, or an `LD_PRELOAD` shim over the
   `open`. Both are ugly, and if the answer to (1) is no, both are wasted.

Worth an hour before committing days to route 2, because the payoff is
hardware decode with no new library at all. Test it the same way: standalone,
with `dmesg -w` running, before anything touches the client. Treat a Hantro
fault the same as an RKVDEC2 one — it resets the block, and it is not a reason
to keep going.

## A kernel bug to know about: `/proc/mpp_service/supports-device`

**Reading that file oopses the kernel and kills the reader.** Reproduced three
times out of three:

```
$ cat /proc/mpp_service/supports-device
Segmentation fault    (exit 139)

Unable to handle kernel NULL pointer dereference at virtual address 0000000000000008
  ESR = 0x96000006, Exception class = DABT (current EL)
  ... vfs_readv / default_file_splice_read / do_splice_to / do_sendfile
```

`/proc/mpp_service/version` is not much better — it contains a single newline
and no version. `supports-cmd` is the only one of the three that works, which
is how the command set above was read.

In practice a modern MPP probes hardware support by **ioctl**
(`MPP_CMD_QUERY_HW_SUPPORT`, `0x00000000` in the supported list) rather than
through procfs, so this is unlikely to be in the path of route 2. It is
recorded because it is a live way to take down a process by reading a file, it
taints the kernel every time, and it is one more datum on how well-maintained
this firmware's MPP stack is: do not read it, and do not let a debugging script
`cat` its way through `/proc/mpp_service/`.

## Route 2, concretely

Build MPP from a revision that has `hal_h264d_vdpu34x.c` and the `mpp_service`
client, for aarch64 against glibc ≤ 2.32, and load it ahead of the system copy
without ever overwriting it:

1. **Pick a revision.** `rockchip-linux/mpp`, a `develop` branch commit from
   2021 or later — early enough to still build cleanly against a 4.19-era
   `mpp_service` uapi, late enough to contain vdpu34x. The check that matters
   before building anything: `ls mpp/hal/rkdec/h264d/` must show
   `hal_h264d_vdpu34x.c`.
2. **Cross-compile it** in `xcloud-cross` (Debian bullseye, gcc 10.2.1,
   aarch64) exactly as `deps/build-deps.sh` does for the WebRTC stack, and run
   `scripts/check-abi.sh` over the result — the whole reason bullseye is the
   build base is that the device is glibc 2.32.
3. **Install it beside the client**, e.g.
   `/userdata/ports/xcloud/lib/librockchip_mpp.so.0`, and set
   `LD_LIBRARY_PATH` in the launcher only. Never replace `/usr/lib`'s copy:
   the root is a read-only squashfs under a tmpfs overlay, and everything else
   on the device that decodes video uses that library.
4. **Test standalone before touching the client.** `mpp/test/mpi_dec_test` from
   the same build, on `/userdata/test720.mp4`, with `dmesg -w` running. A clean
   run there is the whole gate; if it faults, nothing downstream matters.
5. **Only then wire it up**, as an opt-in flag next to the existing decode
   options, defaulting off, with a fallback to software decode on any failure.

Two things to know before starting. `/proc/mpp_service/version` on this device
is empty (one newline), and `/proc/mpp_service/supports-device` is empty too,
so a newer MPP that probes capability by reading those files may need its
`mpp_service_check` path indulged. And the kernel expects RCB info; a
userspace that never sends `MPP_CMD_SET_RCB_INFO` will at best decode without
the SRAM row cache.

## What it would actually buy

Measured software cost today, per frame at 720p60: **10.7 ms decode, 1.1 ms
convert, 4.9 ms present**, against a 16.58 ms budget — 59.7 fps with roughly
30 % spare. Software decode uses about 36 % of the SoC.

Hardware decode would return roughly 1.4 cores. That matters less for frame
rate, which is already fine, than for three other things:

- **Thermals and battery**, on a handheld that currently runs four A55s at a
  pinned 1.8 GHz `performance` governor.
- **Headroom**, which is currently the entire budget for WebRTC, jitter
  buffering and input. Every hardening feature added since M5 has been paid for
  out of it.
- **Text legibility** ([KNOWN-ISSUES.md](KNOWN-ISSUES.md) §1). A hardware
  decoder emits NV12 in a dma-buf that can go straight to a DRM plane, which
  removes the I420→NV12 conversion entirely and makes the CPU downscale
  filters (`-scale box|sharp`, 1.1–1.9 ms today) affordable without competing
  with decode for the same cores.

None of that is worth breaking a working 60 fps pipeline for. It is worth doing
as an opt-in path once there is a standalone `mpi_dec_test` run that does not
fault the VPU.

## Things checked and ruled out along the way

- **CMA exhaustion** — refuted previously and still refuted; `CmaAllocated`
  never moves during a failing decode, and the relevant IOMMUs are bound, so
  MPP's buffers are scattered pages, not CMA.
- **The RGA 2D block** (`/dev/rga`, `librga.so`) — analysed in
  the M2 display spec (project notes) §5 and correctly rejected: the device's librga is
  the 2017 pre-im2d build with no handle cache, so a blit costs 2–4 ms of
  synchronous latency to save 0.3–0.6 ms of CPU, and an `AVFrame` is not a
  dma_buf so it would take the page-pinning path anyway. Nothing found here
  changes that.
- **DDR frequency scaling** — already optimal, not a lever.
  `/sys/class/devfreq/dmc` is on the `performance` governor at its top
  1056 MHz of 324/528/780/1056.
- **CPU governor** — already optimal. One `policy0` covering all four A55s,
  `performance`, pinned at 1.8 GHz.
- **GPU** — `/sys/class/devfreq/fde60000.gpu` sits at its 200 MHz minimum on
  `simple_ondemand`. Correct: nothing in this client touches the Mali, and
  leaving it idle is free thermal headroom.
