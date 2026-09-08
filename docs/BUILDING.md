# Building

## The short version

```bash
sh scripts/bootstrap.sh --host
```

Docker is the only prerequisite. That builds the toolchain images, the WebRTC
stack and both binaries, skipping any step whose output already exists, so it
is also the right thing to run after a pull.

Drop `--host` if you only want the device binary, or use `--host-only` for the
simulator alone.

## Why it needs a container at all

The device runs **glibc 2.32**, and a binary built against anything newer will
not start — Ubuntu 24.04's glibc 2.39 produces one that dies at load with no
useful message. Debian bullseye ships glibc 2.31 and FFmpeg 4.3.x, which
matches the device's `libavcodec.so.58.91.100` and `libavutil.so.56.51.100`
exactly.

`scripts/check-abi.sh` runs on every artefact and **fails the build** on any
reference to `GLIBC_2.33` or newer, so this cannot go wrong quietly.

## What gets built

| Image | Built from | For |
|---|---|---|
| `xcloud-cross` | `docker/Dockerfile.cross-bullseye` | The fast path: amd64 host, emits aarch64 |
| `xcloud-arm64` | `docker/Dockerfile.arm64-bullseye` | Emulated arm64, needed only for the dependency build |
| `xcloud-host` | `docker/Dockerfile.host-bullseye` | x86-64 simulator |

Both device images are bullseye with gcc 10.2.1, so their object files are
interchangeable — the QEMU-built WebRTC archives link into a cross-built binary
unchanged. A **full rebuild of the client is about 29 seconds** cross-compiled,
against roughly 7 minutes emulated.

`scripts/build-app.sh` picks `xcloud-cross` when it exists and falls back to
the emulated image with a note. `XCLOUD_IMAGE` overrides. `-B` forces a full
rebuild, which you need after changing compiler flags — the timestamp check
cannot see those.

## Doing it by hand

```bash
docker build --load -f docker/Dockerfile.cross-bullseye -t xcloud-cross .

# Once, for the static WebRTC stack (emulated, ~7 min):
docker run --privileged --rm tonistiigi/binfmt --install arm64
docker build --load -f docker/Dockerfile.arm64-bullseye -t xcloud-arm64 .
sh deps/build-deps.sh

sh scripts/build-app.sh
```

Three traps, all of which `bootstrap.sh` handles for you:

- **binfmt registration is lost when Docker's VM restarts.** It shows up as
  `exec /bin/sh: exec format error` from an unrelated step. Re-run the
  `tonistiigi/binfmt` line. This only affects the *emulated* image — the cross
  image never executes anything aarch64, so it keeps working.
- **`docker build` needs `--load`**, or the image stays in the build cache
  where `docker run` cannot find it.
- **Do not pass `--platform` to `docker run`.** The image is already arm64 and
  the flag makes the daemon re-resolve the manifest and fail.

## Notes on the build itself

- **The build fetches nothing.** `deps/build-deps.sh` compiles
  `third_party/libpeer`, which is in this repository. It clones nothing, applies
  no patch, and cannot be changed by an upstream force-push. Do not "update"
  that tree by re-cloning over the top — see
  [../third_party/PROVENANCE.md](../third_party/PROVENANCE.md).
- **C and C++ are compiled separately** and linked with `g++`. Passing `-x c`
  partway through a `g++` command line does not reliably switch the frontend.
- **`libstdc++` and `libgcc` are static**, so the client needs only libdrm,
  libfreetype, libcurl, libavcodec/libavformat/libavutil, libasound, libm and
  libc — all present on the device.
- **`-Wall -Wextra` on both languages, and the build is warning-clean.** Treat
  a new warning as a defect.
- **Fonts are not bundled.** The client reads the device's own
  `/usr/share/fonts/dejavu`, which the stock firmware ships.
- **`libsdl2-dev` is deliberately absent** from the images. Bullseye is
  mid-transition to `archive.debian.org` and some pool files 404; SDL pulled in
  `libgl1-mesa-dev`, and nothing here needs it — video is direct libdrm, input
  is raw evdev, audio is ALSA.
- **All three images pin `bullseye-security` to a `snapshot.debian.org`
  timestamp.** Bullseye left LTS on 2026-08-31 and the live security pool has
  begun dropping superseded files, so an unpinned build fails partway through
  `apt-get install` with a 404 on a package the index still lists. If a *main*
  package starts 404ing too, move that line to the same snapshot; each
  Dockerfile carries the reasoning.

## Versioning

The binary carries a version string from `git describe --tags --always
--dirty`, baked in at build time and printed as the **first line of every
log** and by `-version`:

```
xcloud v1.2.0-4-g8a1c3ef (built Sep  7 2026)
```

`main.cpp` is recompiled on every build for this reason — the version can
change without the file changing, and a binary that misreports its own version
is worse than one that has none. A build from a tarball with no git present
reports `dev`. `XCLOUD_VERSION_STRING` overrides it.

Tag a release with an annotated tag so `git describe` finds it:

```bash
git tag -a v1.0.0 -m "..."
```

## Packaging a release

```bash
sh scripts/package.sh    # -> out/xcloud-rg353ps-<version>.zip
```

An archive that installs on a handheld **with no development machine
involved**: the binary, the launcher, the gamelist entry and its merge script,
an `install.sh` that runs on the device in BusyBox sh, and the licences. Copy
the folder to `/userdata` and run `sh install.sh` over SSH.

It packages whatever is in `out/` rather than rebuilding — shipping something
other than the artefact you tested is precisely the mistake worth avoiding —
but it refuses a binary that is not aarch64 or that fails the ABI check, and
warns loudly when the version is `-dirty`, because such an archive corresponds
to no commit.

## Installing on the device

```bash
sh scripts/setup-device.sh    # once: records the address, verifies it
sh scripts/install.sh
```

`setup-device.sh` writes `device.env` in the repository root, which is
gitignored. Nothing in `scripts/` carries a default address, password or host
key — see [`device.env.example`](../device.env.example) for every setting, or
set one for a single command:

```bash
DEVICE_HOST=192.0.2.10 sh scripts/install.sh
```

Both OpenSSH (`ssh`, with a key) and PuTTY's `plink` (with a password) work;
`DEVICE_TRANSPORT` chooses, and `auto` picks plink when a password is set and
plink is installed.

`install.sh` puts the binary at `/userdata/ports/xcloud/xcloud`, the launcher
at `/userdata/roms/ports/Xbox Cloud.sh`, **merges** its entry into the device's
shared `gamelist.xml`, and restarts EmulationStation. The merge matters — that
gamelist belongs to every port on the device, so overwriting it would delete
PortMaster's metadata and everything else. A device gamelist that does not
parse stops the install rather than being overwritten. `NO_ES_RESTART=1` skips
the restart.

`scripts/push.sh` has the device pull over HTTP, about 35 s for a 20 MB debug
build. If it cannot, it fails in seconds rather than silently taking the
ten-minute base64 path; `PUSH_FALLBACK=1` opts into that deliberately.

## Running on the device

```bash
sh scripts/run-device.sh 100 -title WRECKFEST
```

Pushes, stops EmulationStation, runs under `timeout`, and restarts ES on every
path. ES holds DRM master while running, so it has to be out of the way; the
`timeout` means a wedged modeset clears itself instead of needing the power
button. `scripts/run-remote.sh` is the same without the push, for A/B runs of
one binary.

**Always run under `timeout`.**

### Command-line flags

Every flag overrides the saved setting for one run.

| Flag | Does |
|---|---|
| `-title <id>` | Stream one game immediately |
| `-list` | Print every playable launch id |
| `-console <serverId>` / `-consoles` | Remote play against your own Xbox; list consoles and power state |
| `-tier 720\|720hq\|1080\|1080hq` | Stream quality |
| `-locale <BCP-47>` | Streamed console's system language |
| `-res <w>x<h>`, `-bitrate <kbps>` | What the service is asked to encode |
| `-scale hw\|box\|sharp` | Who halves 1280x720 onto the panel |
| `-layout labels\|xbox`, `-swapxy`, `-noswapxy` | Face-button mapping |
| `-threads slice\|frame2\|frame3`, `-reserve N`, `-nort` | Pacing and decode threading |
| `-latch <ms>` | Commit this long before the predicted vblank (lower latency, opt-in) |
| `-nowifi` | Leave Wi-Fi power save and background scan alone |
| `-record <file>` / `-replay <file>` | Save a session / play one back through the pipeline |
| `-autoplay` | Move sticks and tap A on a timer, so an unattended run has motion to encode |
| `-p1devicepath <node>` | Gamepad event node (EmulationStation passes this) |
| `-version` | Print the version and exit |

`-alias`, `-osname` and `-display <w>x<h>` are research hooks that move one
signal of the resolution tier at a time — see [RESOLUTION.md](RESOLUTION.md).

The same settings are in the on-device menu: **START** in the library for the
ones read when a stream starts, **SELECT + X** during a stream for the ones
that take effect immediately. Rows marked `*` wait for the next stream.
Settings persist to `options.json` beside the binary.
