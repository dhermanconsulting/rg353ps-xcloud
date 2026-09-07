# Testing

**You do not need the handheld for most work.** The same source builds for
x86-64 against a simulated panel and runs the whole client — sign-in, catalog,
WebRTC, decode, pacing, audio, UI — inside a container.

## The test bench

```bash
sh scripts/test.sh          # everything
sh scripts/test.sh -quick   # the same, minus the two builds
```

One `PASS` / `FAIL` / `SKIP` line per check on stdout with the key numbers,
progress on stderr, logs in `out/host/test/`, and a **non-zero exit on any
FAIL**. This is what a pull request has to pass.

| Check | What it proves |
|---|---|
| `build-app` | The device binary builds and passes the glibc 2.32 ABI check |
| `build-host` | The simulator and `mkstream` build |
| `synth` | A 20 s synthetic 720p60 recording is generated |
| `clean` | That recording replays through the whole pipeline: shown ≥ 95% of decoded, skipped ≤ 30, late ≤ 2, every audio second `fail=0` |
| `impaired` | With jitter, a gap and loss applied: units were dropped, the resync path ran, and it still exited cleanly |
| `scale` | The CPU 2:1 downscale engages and keeps the pipeline whole |
| `ui` | A scripted walk through the library screen produces screenshots without crashing |

Every simulator run gets its own container name and a hard timeout, and a trap
kills all of them on exit — Ctrl-C stops the docker client, not the container,
so this matters.

Two things the simulator logs that are **not** failures: `SCHED_FIFO` is
refused inside a container (`-nort` quiets it), and ALSA's null device reports
`alsa=0-0ms` with the audio servo at `adj<0` throughout.

## The simulator

```bash
sh scripts/build-host.sh
sh scripts/sim.sh <args>
```

`src/video/drm_output_sim.c` replaces the DRM output with a simulated panel and
vblank clock; everything else is the binary the device runs, built from
identical source with no compile-time switch. It runs in the `xcloud-host`
image with ALSA's null device and no pad.

| Flag | Does |
|---|---|
| `-replay <file>` | Play a recording through the real pipeline at its original arrival timing |
| `-impair "jitter=5,gap=30000:150,loss=1"` | Apply a network impairment model to a replay |
| `-fake-catalog 60` | A synthetic library, no account needed |
| `-ui-script "down,down,A,wait:500,quit"` | Drive the menus from a script |
| `XCLOUD_SIM_SHOTS=<dir>` | PNG screenshot of every UI screen |

```bash
sh scripts/mkstream.sh 20     # 20 s synthetic 720p60 (-jitter, -keyint to vary)
sh scripts/pull.sh /userdata/rec.xcau recordings/rec.xcau   # or a real capture
```

### Watching it live, in a browser

```bash
XCLOUD_SIM_VIEW=8080 sh scripts/sim.sh -replay recordings/synth-20s.xcau -scale sharp
```

Open <http://localhost:8080/>. Every presented frame arrives as a **lossless
PNG** (multipart, capped at 30 fps) and the keyboard drives the pad — arrows are
the d-pad, `WASD` and `IJKL` the sticks, `Z X C V` are A B X Y, Enter is Start,
Backspace is Select. So `Backspace`+`C` is SELECT+X, which opens the options
overlay: **you can switch downscale filters and watch the same frame change, at
a desk, with no handheld.**

PNG rather than JPEG on purpose — the point is to see what the scaler did to
the text, and JPEG would add artefacts of its own on top. The present loop only
memcpy's each frame and signals, so a slow browser drops frames and never slows
the thing being measured. One viewer at a time.
`tools/simview-grab.py` pulls a single frame when there is no browser to hand.

## It does full live sessions too

The simulator **streams from the service for real**, with no handheld involved.
Sign-in is the same device-code flow: the client prints a code and a
`microsoft.com/link` URL, and the token lands in `out/host/state/tokens.json`
(under `out/`, so gitignored). Auth, profile, catalog, session creation,
SDP/ICE, DTLS, media, decode, present and audio all work from inside the
container.

That makes any question about what the **backend** does answerable without
hardware.

Two caveats:

- Live sessions **spend the account's cloud hours**, which are capped from
  November 2026.
- **ICE through Docker's NAT is not perfectly reliable.** Of six sessions, one
  failed at the STUN binding and one dropped mid-stream. Retry before
  concluding anything from a single failed run. Remote play is worse here — see
  [REMOTE-PLAY.md](REMOTE-PLAY.md).

## What the simulator cannot tell you

- **Anything about the panel or the SoC.** `drm_output_sim.c` does not
  reproduce VOP2's scaler, so the coarse-text artefact does not appear at all,
  and an x86 container says nothing about the RK3566's decode budget.
- **Real audio.** ALSA's null device only.
- **A real gamepad.**
- **DTLS timing against a console.** Remote play must be tested on hardware.

Anything touching the SDP template, the handshake or the ICE path needs a real
run on the device before it can be believed.

## Two more that need no handheld

```bash
sh scripts/scaletest.sh   # reproduce VOP2's 2:1 scaler in software, compare
                          # with the CPU filters; PNGs in out/host/scaletest/
sh scripts/nv12check.sh   # the NEON downscale kernels against plain C, on x86
                          # and again on aarch64 under QEMU
```

`nv12check.sh` is the only thing that exercises the NEON intrinsics away from
the device, so run it after touching `src/video/nv12.c`.

## The record/replay harness

`-record <file>` writes an `.xcau` capture of a live session; `-replay <file>`
plays it back through the entire pipeline at the original arrival timing.
**Every pacing number in [VIDEO-PACING.md](VIDEO-PACING.md) was tuned this
way** — deterministically, offline, against the same fixture, which is the only
reason those numbers mean anything.

One warning from [PERFORMANCE.md](PERFORMANCE.md): impairing a *real* capture
exaggerates loss damage by roughly a factor of eighteen compared with the
synthetic fixture, because of how keyframes are distributed. Know which fixture
you are using before you draw a conclusion from it.

`scripts/run-remote.sh 95 -replay /userdata/rec.xcau` runs a replay on the
handheld itself, against the real decoder and the real panel.
