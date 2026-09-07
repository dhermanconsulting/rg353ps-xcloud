# M5 pacing: from judder to a new frame every refresh

Work of 2026-09-04/05. Before: video and audio both juddered, and the
counters said the network was clean. After: a live Wreckfest session shows a
new frame on every refresh of 60 fps content, audio sits 30-50 ms behind the
network with no loss, and the whole client uses about one core of four.

## What was actually wrong

Measured first, with per-stage timing added to a once-a-second `pace|` line
(see "The harness" below). One minute of the old loop, Wreckfest, live:

```
pace| taken=61 dec=61 shown=22 unshown=40 batch=0:0 1:0 2:12 3:8 4+:2
      dec=12.3/20.6ms slices=1-1 flip=46.7/116.0 late=22
      cpu=91% main 75 dec 0 worker 6 audio 7
video| au_gap=16.7/19ms >25:0 >50:0
```

Four facts fell out of that line, none of them guessable from the old log:

1. **The stream is single-slice** (`slices=1-1`). libavcodec's slice
   threading, which the decoder asked for with four threads, therefore ran
   on one thread: `dec 0` in the per-thread CPU column, 12 ms per frame
   with spikes past 20 ms, all on the main thread, with three cores idle.
2. **Arrivals were metronomic** (`au_gap=16.7/19ms`, no gaps over 25 ms).
   The network was blameless.
3. **We discarded 30-70 % of decoded frames** (`unshown=40` of 61). The old
   loop drained every queued access unit, decoded each, presented only the
   last and blocked on the flip. One unit per pass fits in a refresh. Two
   do not, so the pass ends on the second vblank, shows one frame, and two
   more units arrive meanwhile. Every backlog depth is a stable state and
   jitter random-walks it upward until the 16-unit queue overflows, forces
   a keyframe, and the big IDR (54 ms to decode) starts the walk again.
   A real play session had logged `overflow=19 pli=20` in three minutes.
4. **Audio was ~270 ms late, not underrunning.** `alsa=46-67ms
   blk=206ms inmax=10`: the audio thread handed each whole batch of
   decoded packets to one blocking write into a 69 ms buffer, so the write
   blocked ~200 ms, ten more packets arrived, and the batch size was as
   stable as the video backlog. When it crossed the 12-packet shedding
   threshold (an earlier session) it shed ENCODED packets, punched sequence
   gaps, and the reorder buffer's 32-packet wait produced the
   underrun/burst oscillation described in the old KNOWN-ISSUES.

## What changed

**Video** ([src/media/video_pipeline.cpp](../src/media/video_pipeline.cpp),
[src/app/main.cpp](../src/app/main.cpp), [src/video/drm_output.c](../src/video/drm_output.c)):

- A decode thread pops access units and decodes every one in order (P-frames
  need every predecessor), `av_frame_ref`-ing outputs into a small queue.
- The decoder uses **frame threading with two threads**. On a single-slice
  stream that is the only threading that does anything; it releases frame N
  when unit N+1 arrives, which at 60 fps costs about 5 ms over a 12 ms
  decode and roughly doubles the headroom. `AV_CODEC_FLAG_LOW_DELAY` had to
  go (it silently forbids frame threading) and `AV_CODEC_FLAG2_FAST` too
  (non-conformant deblocking, no benefit here).
- The main thread is a **vsync-paced presenter**: wait for the flip event,
  pick a frame, convert to NV12 (0.8 ms), commit, publish the pad, wait
  again. `drm_out_present` was split into `drm_out_commit` and
  `drm_out_wait_flip`; a held picture is re-committed so every refresh ends
  on exactly one flip event; each commit carries a sequence number so a
  flip we timed out on can never be mistaken for the next one.
- **Pick policy**: present as soon as a frame exists (`-reserve 0`); skip
  one frame per refresh while the oldest has waited more than two refreshes
  (drift and decode spikes); jump only when eight or more are queued (a
  stall). Measured on the recording: reserve 0 beat reserve 1 (154 holds /
  12 skips against 165 / 22) because the frame-threaded decoder already
  holds a frame in its pipeline.
- The pad send moved **off the present thread**: it publishes a snapshot,
  the libpeer worker sends it at 125 Hz with the existing idle suppression.
  Nothing on the present thread can block on `peer_mutex_` any more.
- **SCHED_FIFO** for the present thread (12) and the audio thread (15),
  restored to normal when the stream ends so the next stream's threads do
  not inherit it. Wake-up latency after the flip interrupt: 0.1-0.5 ms.
- **cpufreq governor** pinned to `performance` for the stream (was
  `interactive`), restored on exit.
- The refresh interval is now measured from the kernel's own flip
  timestamps: **16.58 ms, i.e. the panel runs at 60.3 Hz**, slightly faster
  than the 60 fps source. One repeated frame every ~3 s is therefore the
  physics, not a fault; the pace line shows it as `held=1`.

**Audio** ([src/media/audio_player.cpp](../src/media/audio_player.cpp),
[src/media/audio_jitter.cpp](../src/media/audio_jitter.cpp)):

- Decoded PCM goes into a ring; each loop writes `min(avail, ring)` so a
  write never blocks (`blk=0ms`). Latency is bounded by trimming decoded
  PCM (never encoded packets) above 160 ms, and a servo drops or repeats
  one sample per 960 while the low-passed depth sits outside 25-55 ms.
- The reorder buffer's gap wait is bounded by time (40 ms), not by 32
  packets.
- The codec is opened **directly at 48 kHz** (`plughw:0,0`, buffer 200 ms,
  period 20 ms, start at 60 ms). The stock `default` device is
  plug -> softvol -> dmix with the slave pinned at 44.1 kHz, which
  resamples everything, quantises the queue depth to 23 ms periods, and on
  the recording drifted 0.14 % against the stream. Bypassing softvol means
  the volume keys' `Master` control is read through the mixer API and
  applied in the sample loop. `default` remains the fallback if the card is
  busy.
- Explicit hw/sw params instead of `snd_pcm_set_params`, whose start
  threshold was the whole buffer.

## Results

| | old loop (live) | new pipeline (live, 102 s) |
|---|---|---|
| frames shown per second, 60 fps content | 19-46 | 60-61 |
| decoded frames never shown | 30-70 % | `skip=10` in the run |
| flip interval | 27-55 ms avg, up to 149 | 16.58 ms flat, `late=1` (the transition) |
| queue overflows / keyframe requests | 19 / 20 in 3 min | 0 / 1 |
| decode | 12 ms, one thread | ~0.9 core across two |
| audio queued | ~270 ms, blocking writes | 29-53 ms, `blk=0ms` |
| audio loss / underruns | lost=888, under=51/min | lost=0, under=1 (start) |
| total CPU | 91 % of one core | 84-112 % of one core |

Wreckfest's menus and intro run at 30 fps; the pipeline shows those at
exactly two refreshes per frame (`hold=0/31/0/0`), which is correct.

## The harness

Every experiment above ran offline on the device against a recording, no
account or live session needed:

- `-record <file>` saves every access unit and audio packet with its
  arrival time (`src/media/au_recorder.hpp`; ~1 MB/s).
- `-replay <file> [-speed f]` feeds a recording through the same decode
  thread, presenter and audio player with the original timing, and prints
  a `replay| SUMMARY` line: decoded, shown, held, skipped, late.
- `-threads slice|frame2|frame3`, `-reserve N`, `-nort` for A/B runs.
- `-autoplay` wiggles the sticks and taps A so an unattended live run has
  motion to encode.
- `scripts/run-remote.sh` runs the binary already on the device with
  different flags; `PROBE=<s>` snapshots the sound card's real hw_params
  and pointer counters mid-run. `tools/xcau-cadence.py` (runs on the
  device) prints a recording's per-second arrival cadence.
- The `pace|` line, once a second from the present thread:
  `shown/held/skip`, hold histogram, queue depth, decode submit time
  (under frame threading that is backpressure, not decode cost), keyframe
  count and decode time, slice count, convert/commit/shadow/wake times,
  flip interval from kernel timestamps, late refreshes, and CPU per thread.

Recordings on the device: `/userdata/rec-wreckfest.xcau` (49 MB, the one
every number above was tuned on) and `/userdata/rec-wreckfest2.xcau` (68 MB,
the final live run).

## The harness without the device: the host simulator

The same client builds for x86-64 and runs inside the `xcloud-host` Docker
image with a simulated panel (`src/video/drm_output_sim.c`: a 60.3 Hz
vblank clock, two malloc'd NV12 buffers, PNG screenshots), ALSA's null
device, no pad and the WebRTC stack built natively into deps/host. Because
every object is compiled from exactly the source the device runs, the
decode thread, presenter, jitter buffers and audio player are the real
ones; only the vblank, the sound card and the pad are stand-ins.

- `sh scripts/build-host.sh` (once: `docker build --load -f
  docker/Dockerfile.host-bullseye -t xcloud-host .`), then
  `sh scripts/sim.sh <client args>`.
- **`XCLOUD_SIM_VIEW=8080` makes it watchable**: a small HTTP server
  (`src/video/sim_view.c`) serves every presented frame to a browser at
  `http://localhost:8080/` and turns the keyboard back into pad state, so
  the options overlay opens with Backspace+C (= SELECT+X) and a downscale
  filter can be judged by eye at a desk. Frames go out as PNG parts of a
  `multipart/x-mixed-replace` response -- the MJPEG trick with a lossless
  codec, deliberately: the thing being looked at is what the scaler did to
  small text, and JPEG would layer its own artefacts on that. Capped at
  30 fps, one viewer at a time. The present loop only memcpy's the frame
  under a mutex and signals; conversion and zlib happen on the streaming
  thread, so a slow browser drops frames instead of slowing the
  measurement. `tools/simview-grab.py` pulls one frame off the stream
  when there is no browser to hand.
- `sh scripts/mkstream.sh 20` writes recordings/synth-20s.xcau: 20 s of
  synthetic 720p60 H.264 (libx264, Constrained Baseline, one slice, no
  B-frames, ~8 Mbps, an IDR every second, a moving scene with a frame
  counter) and 20 ms Opus packets, with ideal arrival timing (`-jitter`,
  `-keyint` to vary). The device's real recordings are pulled with
  `scripts/pull.sh` when it is reachable.
- `-impair "<spec>"` puts a network impairment model between a recording
  and the pipeline: `jitter=<ms>` Gaussian arrival jitter, `gap=<at>:<len>`
  a stall that releases everything at once (what Wi-Fi power save and
  background scans do), `loss=<pct>` drops a unit and everything after it
  until the next IDR (what the live path does after a failed retransmit),
  `burst=<n>` runs of losses; seeded from the spec so runs repeat.
- `-quit-after <s>` and `XCLOUD_SIM_TIMEOUT=<s>` bound every run;
  `XCLOUD_SIM_SHOTS=<dir>` writes every UI frame and every 60th video frame
  as PNG; `-ui-script "down,down,A,wait:500,quit"` drives menus without a
  pad; `-fake-catalog N` shows a synthetic library without an account.
- `sh scripts/test.sh` runs both builds, a clean and an impaired replay
  and a UI smoke test with pass/fail thresholds.

Measured in the simulator (2026-09-05, x86-64):

```
clean:     replay| SUMMARY 20.0s decoded=1199 shown=1199 held=6 skipped=0 late=1 fps=60.0
           lat=38-47 ms average, 50 max (unit complete -> vblank)
impaired:  -impair "jitter=5,gap=8000:150,loss=1"
           replay| SUMMARY 20.0s decoded=836 shown=825 held=379 skipped=11 late=1 fps=41.3
           lost=13 resync=11 discarded=350: thirteen loss events, ~0.5 s each
           until the next IDR; the 150 ms stall became one 8-frame jump
```

The impaired figures are what the loss-hardening changes below are
measured against; the number to drive down is `discarded` per loss event,
which on a live link is the PLI round trip.

## Live play: the Wi-Fi link is now the bottleneck

A five-minute Wreckfest race played from the Ports menu (2026-09-05, log
pulled from the device) with the pipeline above: perfect while it is perfect,
then hitches every 10-20 s and the odd hang. Local side stayed clean the
whole time (`late=0`, wake-ups under 0.5 ms, decode ~1.1 cores at full
race load, a full-screen camera spin costs nothing). The network side:

```
video| ... drop=25 nack=64 resync=84 pli=5 overflow=0
audio  ... lost=250 under=28
au_gap max per second: 51-283 ms in ~40 of 300 seconds; 509, 727 and two
gaps of ~2000 ms
```

The 2 s freezes were a lost frame, a PLI, the answering IDR lost as well,
and then a 1 s wait for the PLI throttle. On the device itself:

- the rtl8821cs driver runs in maximum leisure power save (`ps_info`:
  "LPS mode: MAX", entered 222 times during the session);
- connman configures wpa_supplicant to background-scan every 30 s whenever
  the signal is below -45 dBm, and it sits at -67 dBm; a single-radio SDIO
  chip leaves the channel to scan;
- the driver transmits at 20 MHz (`curr_tx_bw : 20MHz` in dmesg). This is
  NOT a 2.4 GHz link, as an earlier revision of this note guessed: the
  chip is dual-band (`/proc/net/rtl8821cs/wlan0/hal_spec`: band_cap 2G 5G,
  bw_cap 20M 40M 80M) and is associated to the 5 GHz SSID on channel 64
  with an 80 MHz operating width (`rf_info`: cur_ch=64 cur_bw=2). The rate
  adaptation has narrowed transmit to 20 MHz for the signal it has:
  -63 to -67 dBm, signal quality 55 % (`rx_signal`, `survey_info`,
  measured 2026-09-05 at a desk, not in the hand).

What changed in the client for a lossy link:

- **Wi-Fi tuning at stream start** ([src/net/wifi_tune.c](../src/net/wifi_tune.c)):
  power save off through nl80211 `SET_POWER_SAVE` (the wireless-extensions
  ioctl is a stub in this driver), and scanning refused at the driver
  through `/proc/net/rtl8821cs/wlan0/scan_deny` for as long as the stream
  runs; both logged and both put back on exit, `-nowifi` skips it. The
  background scan the supplicant runs is `simple:30:-65:300` (read over
  D-Bus on 2026-09-05): a scan every 30 s whenever the signal is below
  -65 dBm, and the device sits at -63 to -67. wpa_supplicant runs
  D-Bus-only on this firmware (`wpa_supplicant -u`, no control socket),
  so the first version of this, which cleared bgscan with `wpa_cli`, could
  never have taken effect here; that path stays as a fallback for an OS
  build that has a socket. With scan_deny raised the driver answers a scan
  request at once from its cache (`connmanctl scan wifi`: 42 ms and no
  channel departure, against 4.7 s for a real scan), so the supplicant is
  none the wiser. A crash lowers scan_deny from the fatal-signal handler;
  a SIGKILL does not, and `echo 0 > .../scan_deny` or a reboot puts it back.
  Verified 2026-09-05 on the device: `wifi: power save off on wlan0
  (nl80211)` and the scan_deny line at stream start, the driver's LPS
  entry counter unchanged at 38 across a 90 s stream (it had risen by
  about 20 during the 75 s run before the netlink buffer fix, when the
  family lookup was failing), scan_deny lowered on exit; 3944 frames,
  drop=0 nack=0 overflow=0, audio lost=0 under=0, latency 38-68 ms,
  decode up to 122 % of a core in busy scenes, 56 C.
- **Jitter-buffer hold 200 -> 120 ms**, and a **second NACK at 70 ms** for
  whatever the frame at the head is still missing: a NACK or its
  retransmit can itself be lost and there was no second chance (25 of 64
  NACKed frames were still dropped). The hold is the length of every
  freeze that no retransmit repairs.
- **PLI throttle 300 ms, doubling to 2 s** while an IDR is outstanding,
  and only asked for while the assembler is waiting for one: a lost IDR
  costs 300 ms rather than a second, without the 3 IDR/s storm a flat
  300 ms would invite on a link that is losing packets.
- **Bandwidth adaptation through REMB**: a second is lossy on two dropped
  frames or five NACK messages; two lossy seconds in a row, or one bad one,
  back the cap off 15% (floor 3 Mbps) with a 3 s hold-off so one event is
  not counted twice while the encoder catches up; 1 Mbps back per five
  clean seconds. `remb=` in the `video|` line.
- **Audio loss concealment**: a packet the reorder buffer gives up on (now
  after a 5 ms grace, not 40: audio has no retransmission and the wait was
  draining the card) is reconstructed by libopus packet-loss concealment
  (`libopus.so.0` loaded at runtime, FFmpeg's decoder as the fallback with
  silence fill) instead of causing an underrun, a restart and a re-prime.
  The card sits 45-75 ms deep to cover the detection delay; latency trim
  threshold 160 -> 120 ms so post-stall bursts are cut back to 60 ms.
- **Network thread at SCHED_FIFO 8** so the socket is emptied the moment a
  packet lands even when the decoder has two cores busy.

Still the user's to fix: signal. The device is already on 5 GHz (channel
64, the quietest radio in the house: 3 % busy against 40-50 % on the
2.4 GHz channels), so the number to raise is the -65 dBm it sees from the
nearest access point: closer to it, or another AP in the room. A static
DHCP lease would stop the address moving. If nl80211 power save turns out
not to take, the fallback is a persistent `options 8821cs rtw_power_mgnt=0
rtw_ips_mode=0` in /etc/modprobe.d saved with `anbernic-save-overlay`.

## Input

- **Face buttons follow the printed labels** by default (`-layout xbox`
  for Xbox positions). The device tree names the GPIOs by Xbox position
  (bottom emits BTN_SOUTH, the code named BTN-A) while the shell is
  labelled Nintendo-style, so the old table gave positional Xbox mapping;
  pressing the button that says A now sends Xbox A. The stick Y axes were
  already right: the wire format negates them.
- **Pad polling on its own thread** during a stream (blocking poll, 8 ms
  timeout), published to the engine and sent by the worker at up to
  125 Hz, instead of one sample per 16.7 ms refresh.
- An end-to-end latency figure, `lat=` in the pace line: the arrival of a
  unit's last packet to the vblank that showed it.

## The downscale: why text suffers and games do not

Reported 2026-09-05: "the games look great but text suffers". The pacing
work above put a new frame on the panel every refresh; this is about what
is *in* that frame.

Until now nothing in the client scaled anything. The plane took a full
1280x720 NV12 buffer with a 640x360 destination rectangle and VOP2's Esmart
scaler did the halving. Reading `vop2_setup_scale()`, that scaler is a poor
downscaler in two separate ways:

- **Horizontally** it is a 2-tap bilinear whose step is
  `DIV_ROUND_UP((1280-1) << 12, 640-1) - 1 = 8198`, i.e. 2.00146 source
  pixels, with the phase starting at 0. The two taps are therefore weighted
  1.000/0.000 at the left edge, 0.883/0.117 an eighth of the way in,
  0.531/0.469 in the middle and 0.064/0.936 at the right edge. Over most of
  the line one source column is taken and its neighbour discarded, and
  **which parity survives drifts across the picture**.
- **Vertically** 720 >= 2*360 engages the GT2 pre-scaler, `src_h` becomes
  360, and the vertical scaler then sees 360 -> 360 and is bypassed. The
  whole vertical resample is that one hardware stage, and whether GT2
  averages two lines or keeps one is not stated by the driver.

That asymmetry is the answer to "why text and not games". Photographic and
3D content has no thin high-contrast feature whose loss you would notice.
Glyphs are 1-2 pixel stems at 720p, so each stem survives, vanishes or
doubles according to its parity -- and the parity changes as the picture
pans, which is why it reads as unstable rather than merely soft.

`tools/scaletest.c` (`sh scripts/scaletest.sh`) reproduces the hardware
filter and puts it beside the alternatives, needing no device: PNGs in
`out/host/scaletest/`. Mean absolute error against a Lanczos-3 reference
over a block of HUD-sized text, at the left and the right of the frame:

| filter | left | right |
|---|---|---|
| VOP2 today, GT2 dropping a line | 7.12 | 5.49 |
| VOP2 today, GT2 averaging two | 5.28 | 3.60 |
| CPU 2x2 box | 2.59 | 2.59 |
| CPU sharp, `[-1 9 9 -1]/16` | 4.10 | 4.10 |
| CPU box in linear light | 3.60 | 3.60 |

The left/right split for the hardware, and its absence for every CPU
filter, is the phase drift as a number. `sharp` scores worse than `box`
only because it deliberately departs from the reference by adding
acutance; for text that is the point, so it is judged by eye.
`compare-patterns.png` is the plainest evidence: with GT2 dropping, a band
of 1-pixel rows on even y renders as solid white and on odd y as solid
black.

### What changed

`-scale hw|box|sharp` (`nv12_scale2_luma`, `nv12_scale2_chroma` in
src/video/nv12.c, wired up in `prepare_frame`). `box` is a 2x2 average;
`sharp` is a separable `[-1 9 9 -1]/16`, which is Catmull-Rom at the
half-pixel phase and sums to 16, so flat areas are untouched and only
edges change. Chroma is always a box: it is already at half resolution and
sharpening it only rings colour edges. Both are NEON with scalar
fallbacks.

The CPU path engages **only when halving lands exactly on the rectangle
the frame would have been letterboxed into** -- the 1280x720 -> 640x360
case. Any other source size keeps the old path, so a mid-session
resolution change cannot leave the picture the wrong shape. The plane then
gets a 640x360 source and a 640x360 destination and scales nothing at all.
It also writes a quarter as many bytes into the write-combine mapping,
which is where the old full-size copy spent its time.

**Default is `hw`, unchanged, until a device A/B says otherwise.**

### What is verified, and what is not

- The NEON kernels match plain C byte for byte on aarch64 under QEMU, at
  four sizes including widths that are not multiples of 16 and one too
  small for the vector loop: `sh scripts/nv12check.sh`. This was worth
  doing before a device trip -- the host simulator only ever runs the
  scalar path, so nothing else exercises the intrinsics.
- The pipeline survives it: `test.sh`'s `scale` check replays with
  `-scale sharp` and requires the path to engage and shown/late to hold.
- **Measured on the device**, live Wreckfest at a real 60 fps, `cvt=` from
  the pace line (mean/max, ms per frame):

  | -scale | cvt | shown | late |
  |---|---|---|---|
  | hw (VOP2) | 0.7 / 0.8 | 61/61 | 0 |
  | box | 1.1 / 1.6 | 60-61/61 | 0 |
  | sharp | 1.9 / 2.1 | — | 0 |

  So the CPU path costs 0.4 ms a frame for box and about 1.2 ms for sharp,
  against a 16.58 ms budget, and neither cost a single late refresh or a
  dropped frame. The 4x cut in write-combine traffic pays for most of the
  box; sharp's extra pass is what the rest buys. (The sharp figure was
  taken while the stream was sending ~25 fps, so its total CPU share is
  not comparable with the other two rows; the per-frame cost is.)
- **Not settled**: whether GT2 averages or decimates. It does not change
  the fix, only how large the win is.

### Asking xCloud for a smaller picture instead

The other end of the same problem: at 640x360 the panel shows a quarter of
the pixels the server encodes, and the 8 Mbps is spent on all of them.
`startup_messages()` already declares `supportsCustomResolution` in both
`clientdevicecapabilities` and `dimensionschanged`, so a size other than
the tier's own is at least expressible. `-res <w>x<h>` and `-bitrate
<kbps>` put those numbers under a flag (`Engine::set_requested_video`).

`-res 640x360` would have removed the downscale altogether and spent the
whole bitrate on the pixels the panel shows, which should beat any
client-side filter.

**Tested on the device 2026-09-05: xCloud ignores it.** The log reads

```
asking for 640x360 at 8000 kbps
xcloud: stream is now 1280x720
```

and the session was otherwise entirely normal -- 60 fps, `late=0`, no
error, no renegotiation. So `supportsCustomResolution` with a smaller
`maxWidth`/`maxHeight` does not get a smaller encode out of this service;
it is presumably there for aspect ratio and safe areas on phones rather
than as a way down from the tier. The `userRequestedResolutionUpdate`
alias cannot help either: its enum is Auto / 720 / 720HQ / 1080 / 1080HQ /
1440, with nothing below 720.

### Then why not, and what is left to try

Three levers, two tested and dead.

**1. The capability messages -- ignored.** Above. Worth knowing: the other
open clients (greenlight, xbox-xcloud-player, xcloud-rs) send
`clientdevicecapabilities` as an *empty object* and hardcode
`dimensionschanged` at 1920x1080 regardless of their real window. green-nx
is the one that fills in `maxWidth`/`maxHeight`/`maxBitrateKbps`. Nobody
appears to get a resolution out of it, which fits: those fields look like
they are for aspect ratio and safe areas on phones, not for picking an
encode size.

**2. Starving the encoder -- also ignored.** The likelier mechanism, since
a libwebrtc-shaped sender drops resolution when the target bitrate will not
carry it, and since xCloud demonstrably *does* change resolution
mid-session. REMB is what the sender tracks second by second, so this is
the number that should matter.

Note that `-bitrate` originally only changed the capability message; the
REMB cap was still `tier_profile(tier_).bitrate_kbps`, so the flag was
half-wired. Fixed, then tested: **110 seconds at an advertised 1200 kbps
and the stream never left 1280x720**, at a full 60 fps throughout, with one
`stream is now` line for the whole run. So xCloud's degradation preference
is "maintain resolution": it spends the shortfall on quality, not on
pixels. That also means a weak Wi-Fi link makes text worse *without* making
the picture smaller, which is worth remembering.

**3. The device fingerprint -- untested, and the one that is left.** The
tier is not requested; it is *inferred*. `session.cpp` picks
`osName` per tier -- android for 720p, windows for 1080p, tizen for
1080pHQ -- and sends `X-MS-Device-Info` carrying
`displayInfo.dimensions.widthInPixels/heightInPixels` set to the tier's
resolution. So the thing that actually selects 720p today is what we claim
to *be* at session-creation time, not anything we ask for later. Declaring
a 640x360 display there is the remaining experiment.

It needs a change inside `reference/green-nx/src/core/session.cpp`, which
is a read-only upstream clone, so it wants a deliberate patch (the
precedent is `deps/patches/libpeer-rg353.patch`) rather than a quiet edit.
It also carries a real risk the file itself warns about: an unusual size
may land the session in an unknown-device profile server-side rather than a
smaller one.

**Until then the client-side downscale is the fix**, not a stopgap. The
flags stay: they cost nothing, the answer may not be permanent, and
`-bitrate` is independently useful because at a fixed 720p, bits per pixel
is the other thing that decides whether small text survives.

## The options menu

Everything above is a flag, and a flag means a rebuild, a push and a fresh
session to try the other value. `src/app/options.cpp` puts the same
switches behind a menu, in one table, so adding a switch is a row there and
nothing else.

Two ways in. **SELECT + X during a stream** draws it over the running
picture, which is the point: the downscale filter and the late latch take
effect on the next frame, so box against sharp is a comparison of the same
scene a second apart rather than of two sessions. **START in the library**
is the same table full-screen, for the ones that are read when a stream
starts (the requested size, the bitrate cap, decode threading, Wi-Fi
tuning, real-time priority); those are marked `*` and the menu says so.
Values live in `<state dir>/options.json`; `options_load()` runs before the
command line is parsed and `options_capture()` after, so a flag always wins
and the menu still opens showing what is actually in force.

The overlay is drawn into the frame already in the back buffer, between the
conversion timing and the commit timing, so having it open inflates neither
`cvt` nor `commit` -- comparing `cvt` between filters is exactly what it is
for. It is write-only, like every other blit here: a panel from
`screen_rect_colour` (which also neutralises the chroma under it, or the
video's colour would tint the menu) and glyphs on top. Font size is chosen
from the *buffer* height, not the panel's, because under the hardware
downscale everything in a 1280x720 buffer is halved before it reaches the
eye.

While the menu is open the input thread sends a neutral pad frame, or
choosing an entry would also steer the car.

Verified live on the device, switching the filter mid-stream with a
`-ui-script`: `cvt` steps 0.7 -> 1.0 -> 1.9 ms as hardware -> box -> sharp
with no other change to the run. The one rough edge is the switch itself:
changing the downscale changes the buffer size, so the plane's buffers are
reallocated and that single frame costs about 20 ms, the same hitch as a
mid-stream resolution change (issue 2 in KNOWN-ISSUES). Acceptable in a
menu you opened on purpose; it is the same fix if it ever matters.

## Deferred, from the design review

Three independent reviews of the design ran before implementation; what
was taken is above. Left for later, in order of value:

- **Resolution change blanks the plane.** `drm_out_set_source` removes the
  framebuffer that is scanning out, which forces a synchronous plane
  disable (one black frame, a 20-40 ms stall). Fix: allocate the new-size
  buffers first, commit one, wait for its event, then free the old pair.
- **Late latch.** Committing right after the flip event costs a full
  refresh of decision-to-photon latency; picking, converting and committing
  ~5 ms before the predicted vblank would save ~10 ms. Needs the
  commit-to-event time measured first (the driver programs the plane from
  a worker on the same CPU, and an occasional `commit=8.6ms` in the live
  run is that worker being late).
- **Packet loss concealment.** libavcodec's Opus decoder has none; a lost
  packet is a 20 ms hole. libopus is on the device and would give PLC and
  FEC in ~40 lines.
- **Direct audio device when EmulationStation holds the card.** From the
  Ports menu ES stays alive and its dmix client may keep `hw:0,0` open;
  the player then falls back to `default` and logs it. Verify from the
  menu; if it always falls back, close ES's audio in the launcher.
