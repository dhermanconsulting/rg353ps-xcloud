# Architecture

How the client is put together, and why it is shaped this way.

## The constraints, first

Every decision below follows from four facts about the target, all measured —
see [DEVICE.md](DEVICE.md):

1. **1 GB of RAM, four Cortex-A55 at 1.8 GHz.** Not much headroom.
2. **Hardware video decode does not work on the stock firmware.** Not a tuning
   problem — the firmware's Rockchip MPP library has no HAL for the decoder
   block this SoC has. [HARDWARE-DECODE.md](HARDWARE-DECODE.md) has the
   evidence.
3. **Software decode is fast enough anyway**: 720p60 at 2.54x realtime, about
   36% of the SoC. In the live pipeline, 10.7 ms decode against a 16.58 ms
   frame budget.
4. **No X11, no Wayland, no browser, no compiler, no SDL worth using.** The
   panel is reached through DRM/KMS directly, the pad through raw evdev, audio
   through ALSA.

So: a single statically-inclined native binary, no runtime, no toolkit, with
roughly a third of each frame's budget spent on decode and the rest spent
carefully.

## The shape

```
                  ┌───────────────────────────────────────────────┐
   Microsoft      │                  src/gnx/                     │
   GSSV REST  ───►│  auth · catalog · session   (GPL-3.0)         │
                  └───────────────────┬───────────────────────────┘
                                      │ SDP, ICE credentials, session id
                  ┌───────────────────▼───────────────────────────┐
   WebRTC     ◄──►│                  src/net/engine               │
   DTLS-SRTP      │  libpeer + signalling + 4 data channels        │
                  └────────┬──────────────────────┬───────────────┘
                    RTP video               RTP audio        input ▲
                           │                      │                │
                  ┌────────▼─────────┐   ┌────────▼────────┐   ┌───┴────────┐
                  │  src/media/      │   │  src/media/     │   │ src/input/ │
                  │  video_jitter    │   │  audio_jitter   │   │ evdev_pad  │
                  │  decoder (SW)    │   │  audio_player   │   └────────────┘
                  │  video_pipeline  │   │  (ALSA)         │
                  └────────┬─────────┘   └─────────────────┘
                           │ NV12 frames
                  ┌────────▼─────────────────────────────────────┐
                  │  src/video/  NV12 convert · 2:1 downscale     │
                  │              DRM overlay plane, vsync-paced   │
                  └──────────────────────────────────────────────┘
```

`src/app/` sits over all of it: the main loop, the library screen, the options
menu and the presenter.

## Threads

Four, and the division is the whole performance story
([VIDEO-PACING.md](VIDEO-PACING.md)):

| Thread | Does | Why separate |
|---|---|---|
| **main** | Presents frames, paced to vblank; drives UI | Must hit every 16.58 ms refresh, so it cannot block on anything |
| **decode** | Software H.264, frame-threaded | 10.7 ms per frame — far too long to sit on the present path |
| **network** | libpeer worker: ICE, DTLS, RTP, RTCP | Its timing drives keyframe recovery and consent; jitter here is loss |
| **audio** | ALSA writes from the audio jitter buffer | An underrun is audible; a late frame is not |

The presenter never waits on the decoder. It takes the newest complete frame
and re-shows the previous one if there is not a new one — which is why a
network hiccup costs a held frame rather than a stall.

## Module by module

### `src/gnx/` — account, catalog, session (GPL-3.0)

From [green-nx](https://github.com/rmrf404/green-nx), and the reason the whole
client is GPL-3.0. Handles the Microsoft device-code login, the Xbox Live token
exchange chain, the Game Pass catalog, and session creation against the GSSV
endpoints — including the `xhome` offering used for remote play.

It is in `src/` rather than `third_party/` because it is modified: see
[../src/gnx/PROVENANCE.md](../src/gnx/PROVENANCE.md). The device fingerprint in
`session.cpp` is what tells the service which resolution tier to encode at, so
it is a file with live decisions in it, not inert vendored code.

The REST chain it walks is documented in [PROTOCOL.md](PROTOCOL.md).

### `src/net/` — the stream engine

`engine.cpp` is the largest single file in the project (~1,900 lines) and owns
the whole live session: it takes the SDP offer from libpeer, munges it into the
shape xCloud accepts, drives the ICE and DTLS handshake, then pumps RTP into
the jitter buffers and RTCP feedback back out.

Four data channels ride the SCTP association — input, control, message and
chat — with per-stream reliability settings, because the input channel must be
reliable and ordered while the others need not be.

The RTCP side matters more than it looks: **without valid receiver reports, a
libwebrtc sender pins its encoder near the starvation floor**. NACK, PLI, REMB
and Receiver Reports with LSR/DLSR are all implemented, and the vendored
libpeer was modified to make that possible.

`wifi_tune.c` switches off Wi-Fi power save (via nl80211) and background
scanning (via the Realtek driver's `scan_deny`, falling back to `wpa_cli`) for
the duration of a stream, and restores both on exit. The link is the system's
real bottleneck, so a scan mid-stream is a visible glitch.

### `src/media/` — decode and pacing

`decoder.c` wraps FFmpeg's software H.264 decoder with configurable frame
threading. `video_jitter.cpp` and `audio_jitter.cpp` do reassembly, reordering
and keyframe gating on raw RTP — libpeer's own NALU assembly corrupts frames on
any reorder, so it is bypassed entirely and the whole packet, header included,
is handed here.

`video_pipeline.cpp` runs the decode thread and hands complete frames to the
presenter. `audio_player.cpp` is Opus into ALSA with concealment on loss.

`au_recorder.cpp` writes the `.xcau` capture format, which is what makes the
offline harness possible: a real session recorded once can be replayed through
the entire pipeline, deterministically, for ever. Every pacing number in
[VIDEO-PACING.md](VIDEO-PACING.md) was tuned that way.

### `src/video/` — getting pixels onto the panel

`drm_output.c` drives a DRM **overlay plane** with `drmModeAtomicCommit` and
`DRM_MODE_PAGE_FLIP_EVENT`. Three traps are encoded in it, each of which cost
real time to find:

- **`zpos` is a lie on this driver.** The property is mutable, the write
  succeeds, debugfs echoes the new value, and every plane still reports
  `normalized-zpos=0` because this BSP never calls
  `drm_atomic_normalize_zpos()`. Do not order planes by zpos — switch the
  occluding plane off and restore it on exit.
- **Legacy `drmModeSetPlane` is unusable**, at 3048 ms per frame.
- **`drmWaitVBlank` needs the CRTC index** in the high bits, or it assumes
  pipe 0 — a disabled video port on this board — and returns `EBUSY` every
  time.

`nv12.c` is the colour conversion and the 2:1 downscale, with NEON kernels
checked against a plain-C reference by `scripts/nv12check.sh`.

`drm_output_sim.c` is the same interface against a simulated panel and vblank
clock, which is what lets the whole client run on a development machine.
`sim_view.c` is a small HTTP server that streams every presented frame to a
browser as a lossless PNG — see [TESTING.md](TESTING.md).

### `src/app/` — the client itself

`main.cpp` is argument parsing, setup and the top-level state machine.
`library.cpp` is the catalog screen — box art, search, sort, recently played,
and the remote-play section. `library_data.cpp` handles the on-disk catalog
cache, so the library is not blank on a cold start. `options.cpp` is the
settings menu, one table driving both the in-stream overlay and the full
screen, with staged edits applied on confirm. `present.cpp` is the vsync-paced
present loop. `replay.cpp` plays a recording back through the real pipeline.

## Cross-cutting decisions worth knowing

**Resolution is not ours to choose.** The service encodes at 1280x720 and the
panel is 640x480, so the picture is halved. Every client-side route to a
smaller encode was tried and failed — except the title-side custom-resolution
API, which some games honour and most ignore. The client declares 640x360
because it is free where ignored and native where honoured.
[RESOLUTION.md](RESOLUTION.md) has the matrix.

**The stream can change resolution mid-session.** A stall, a PLI and a fresh
IDR can come back at a different size. Anything caching frame dimensions has a
buffer overrun waiting in it — re-check every frame.

**Nothing falls back silently.** A slow path that engages by itself is
indistinguishable from a hang, which cost real debugging time more than once.
Fast paths fail loudly and name the cause; slow paths are opt-in.

**The build fetches nothing.** Every dependency is vendored, so a checkout plus
Docker is sufficient and no upstream force-push can change what compiles.
[DEPENDENCIES.md](DEPENDENCIES.md), [BUILDING.md](BUILDING.md).
