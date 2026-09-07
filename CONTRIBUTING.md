# Contributing

Thanks for looking. This is a small, opinionated codebase for one family of
devices, so the most useful thing you can do before writing code is say what
you intend to change — an issue first saves both of us a rewrite.

## Getting set up

Docker is the only prerequisite. Nothing is fetched from the network at build
time; the whole dependency stack is vendored.

```bash
sh scripts/bootstrap.sh --host   # toolchain images, WebRTC stack, both binaries
sh scripts/test.sh               # the test bench
```

With a handheld to hand, `sh scripts/setup-device.sh` records its address and
verifies the connection, then `sh scripts/install.sh` puts the client on it.

**You do not need the device for most changes.** The same source builds for
x86-64 against a simulated panel and runs real streaming sessions from a
container — see [docs/TESTING.md](docs/TESTING.md). What the simulator cannot
tell you about is the panel, the SoC's decode budget, or DTLS timing against a
real console.

## Before you open a pull request

1. **`sh scripts/test.sh` passes.** Non-zero exit on any `FAIL`. If your change
   touches pacing, decode or audio, say which thresholds moved and why.
2. **The build is warning-clean.** `-Wall -Wextra` on both C and C++; treat a
   new warning as a defect rather than noise to be silenced.
3. **`sh scripts/check-abi.sh out/xcloud` passes.** It runs as part of the
   build. Any reference to `GLIBC_2.33` or newer means the binary will not
   start on the stock firmware.
4. **Anything touching the stream is tested on hardware.** The SDP template,
   the DTLS handshake and the ICE path are not things the offline harness can
   check. Say in the PR what you ran and what came back.

## House style

Match the file you are editing. Broadly:

- Tabs for indentation in C and C++, as the existing sources use.
- **Comments explain *why*, not *what*.** This codebase is full of decisions
  that look wrong until you know what was measured — `zpos` being a trap on
  this driver, why `drmWaitVBlank` needs the CRTC index, why the fallback in
  `push.sh` is opt-in. If you change one of those, change the comment with it.
  If you are undoing something, check the comment first: it usually says what
  went wrong last time.
- **No silent fallbacks.** A slow path that engages by itself is worse than a
  failure, because it looks like a hang. Fail loudly, name the cause, and say
  how to opt into the slow route deliberately.
- Numbers in comments and docs should be measured, and should say when. "About
  30 seconds" is fine; an invented benchmark is not.
- Anything cached from the wire is re-validated per frame. The service changes
  resolution mid-session, so code that remembers frame dimensions has a buffer
  overrun in it.

## What goes where

| | |
|---|---|
| `src/app/` | The client: main loop, library, options, stream, presenter |
| `src/net/` | Stream engine — WebRTC plus GSSV signalling |
| `src/media/` | Decode, jitter buffers, pacing, ALSA, record/replay |
| `src/video/` | DRM output, NV12 conversion, downscale, the simulator |
| `src/ui/`, `src/input/` | FreeType text, raw evdev gamepad |
| `src/gnx/` | Account, catalog and session. **GPL-3.0, from green-nx.** |
| `third_party/` | Vendored dependencies. Read `PROVENANCE.md` before touching. |
| `docs/` | Reference documentation, kept current with the code |

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) is the fuller tour.

### Two directories with rules of their own

**`third_party/`** is a local copy, not a clone, and libpeer in particular
carries substantial changes that are not upstream. Do not "update" it by
re-cloning over the top — you would silently drop the SDP template, the raw RTP
passthrough and the DTLS fixes, and the failure mode is a stream that connects
and shows nothing. [third_party/PROVENANCE.md](third_party/PROVENANCE.md)
explains how to rebase onto a newer version if that is ever wanted.

**`src/gnx/`** is GPL-3.0 code from green-nx. It is fine to modify — that is
why it lives in `src/` — but record the change in
[src/gnx/PROVENANCE.md](src/gnx/PROVENANCE.md) so the delta from upstream stays
readable.

## Licensing of contributions

By contributing you agree your work is licensed under **GPL-3.0**, the licence
the client as a whole carries. Do not paste in code you cannot license that
way — in particular, nothing decompiled from, or copied out of, a Microsoft
client.

## Scope

Things that are welcome: bug fixes with a reproduction, hardware support for
other RG353 variants, better error messages, documentation that corrects
something wrong.

Things to discuss first: new dependencies (the vendored stack is deliberately
small and the device has 1 GB of RAM), anything that changes the wire protocol,
and anything that would make the client harder to build from a bare checkout.

Out of scope: circumventing account restrictions, region locks, subscription
checks, or any technical protection measure. This is a client for a service you
pay for, and it stays that way.
