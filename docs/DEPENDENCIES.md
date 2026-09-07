# The borrowed code: audit, vendoring, and what the deep dive found

Done 2026-09-05. Two questions: what are we actually compiling that we did not
write, and is any of it doing something wrong on this device.

## Part 1 — what was being compiled from where

Before this, the build reached outside the project in two places.

**green-nx's `src/core/`** — five `.cpp` files, 1671 lines with headers, the
whole xCloud account/catalog/session layer — was compiled **straight out of
`reference/green-nx/`**, a git clone of somebody else's repository sitting in
the tree for reference. `scripts/sources.sh` had `CORE=reference/green-nx/src/core`
and both build scripts added `-I reference/green-nx/vendor` for its bundled
`json.hpp`. Four of our own headers `#include`d across into it by relative path.

A `git pull` in that directory would have silently changed what the client
does. Worse in practice: `docs/KNOWN-ISSUES.md` §1 already identified a
change we want to make there — the device fingerprint in `session.cpp` that
selects 720p over 1080p — and recorded it as blocked because it "needs a patch
to the green-nx clone". A change we want to make, in a file we compile, that we
had no clean way to make.

**libpeer** was not in the tree at all. `deps/build-deps.sh` shallow-fetched a
pinned commit from GitHub at build time, applied a 1191-line patch, initialised
six submodules, and built the result. If that commit ever became unreachable —
a force-push, a repo move, GitHub garbage-collecting an unreferenced object —
the patch would have been 1191 lines of context with nothing to apply to, and
the WebRTC layer would have been unbuildable. The project is not under version
control, so there was no history to recover it from either.

## Part 2 — what moved, and where

| Was | Now |
|---|---|
| `reference/green-nx/src/core/*` compiled in place | `src/gnx/` — ours, GPL-3.0, `src/gnx/PROVENANCE.md` |
| `reference/green-nx/vendor/json.hpp` via `-I` | `third_party/nlohmann/json.hpp` |
| libpeer cloned from GitHub + 1191-line patch | `third_party/libpeer/`, patch pre-applied |
| mbedtls / libsrtp / usrsctp as submodules | `third_party/libpeer/third_party/`, pruned |
| cJSON / coreHTTP / coreMQTT as submodules | gone — see below |

`deps/patches/libpeer-rg353.patch` is kept as the **record** of what was
changed against pristine upstream. Nothing applies it any more. Full detail in
[third_party/PROVENANCE.md](../third_party/PROVENANCE.md).

`reference/` is now what its name says: read-only clones for comparison,
referenced by no build script. Verified:

```bash
grep -rn 'reference/\|deps/src' scripts/ deps/build-deps.sh src/ tools/ cmake/ docker/ port/
```

returns nothing but prose in the provenance files.

**cJSON, coreHTTP and coreMQTT were fetched on every fresh setup and never
compiled.** 5.9 MB and three network round-trips serving libpeer's MQTT/HTTP
signalling, which we build with `-DDISABLE_PEER_SIGNALING=ON`. They are gone.

The three real dependencies were copied whole and then pruned of directories
nothing compiles — mbedtls `tests/` alone was 28 MB — taking `third_party/`
from about 65 MB to 14 MB. The build already passed `-DENABLE_TESTING=OFF`,
`-DENABLE_PROGRAMS=OFF`, `-DTEST_APPS=OFF` and `-Dsctp_build_programs=0`, so
none of it was being built.

Verified end to end: both dependency targets rebuilt from scratch from the
vendored tree (host x86-64 and arm64, ABI clean for glibc 2.32), both clients
relinked, and `scripts/test.sh` passes 7 of 7.

## Part 3 — what the deep dive found

### Verified and fixed: the receive buffer was a twentieth of what we asked for

`third_party/libpeer/src/socket.c` asked for a 4 MB `SO_RCVBUF` so a keyframe
burst would not be dropped before the worker drained it. That hunk is ours —
it is in our patch, not upstream libpeer.

**Linux silently clamps `SO_RCVBUF` to `net.core.rmem_max` and returns
success.** On this firmware `rmem_max` is 212992. Measured on the device:

```
SO_RCVBUF      asked 4096 KB -> usable  208 KB
SO_RCVBUFFORCE asked 4096 KB -> usable 4096 KB
```

So the buffer was 208 KB, not 4 MB, and nothing said so. 208 KB is genuinely
marginal here: at the ~8 Mbit/s this stream runs at, a 200 ms stall in the
worker overflows it, and that is exactly the situation the buffer exists for.

`SO_RCVBUFFORCE` is the same option without the clamp. It needs `CAP_NET_ADMIN`
— which we have, because the client runs as root on this device. Fixed: try the
forced form first, fall back to the clamped one, then **read back what was
actually granted and log it**, because the whole failure mode here was a
request that looked honoured to anyone who did not check.

Set to **1 MB, not 4 MB**. At 8 Mbit/s, 1 MB is about a second of video —
already far longer than a packet can be late and still be worth having, and
about three times the largest keyframe. 4 MB would be four seconds of queue.
The jitter buffer discards stale packets so it would not have broken anything,
but there is no case where holding that much helps, and buffering junk in a
real-time path is how a stall gets reported as latency instead of loss.

Look for `recv buffer: 1024 KB via SO_RCVBUFFORCE` in the log at stream start.

### Untried lever on the coarse-text problem: `max-fs` in the SDP offer

[KNOWN-ISSUES.md](KNOWN-ISSUES.md) §1 says encoding smaller at the source is
unavailable, having tested two ways: the app-level capability messages
(`clientdevicecapabilities`, `dimensionschanged`) and starving the bitrate.
Both are ignored — xCloud spends a shortfall on quality, not on pixels.

There is a third lever neither of those touches. The SDP offer
(`third_party/libpeer/src/sdp.c`) declares H.264 decode capability at the
**codec negotiation** layer:

```
a=fmtp:102 level-asymmetry-allowed=0;packetization-mode=1;
           profile-level-id=42e01f;max-fs=3600;max-mbps=108000
```

`max-fs=3600` is macroblocks per frame — exactly 1280×720. In principle a
libwebrtc sender honours a receiver's declared `max-fs` (RFC 6184), because
ignoring it means sending something the receiver said it cannot decode. 640×360
would be `max-fs=900;max-mbps=54000`.

**But there is direct evidence in this very line that xCloud is not enforcing
the throughput half of it.** `max-mbps` is macroblocks per *second*:
3600 × 60 = 216000 for 720p60, and the offer declares 108000 — exactly half,
i.e. 720p**30**. We nonetheless receive a clean 720p60. Compare
`sdp_scale_video_caps_1080()` in `src/gnx/session.cpp`, which rewrites the pair
to 8160 / 489600 for the 1080p tiers: 8160 × 60, correctly 60 fps. So the 720p
template under-declares its frame rate by 2x, xCloud sends 60 fps anyway, and
that is a measured demonstration that at least `max-mbps` is being ignored.

That materially lowers the odds on `max-fs` too. Two things keep it worth one
test anyway: `max-fs` and `max-mbps` are enforced by different code paths in a
typical encoder (frame size bounds what the receiver can allocate; throughput
does not), and the change costs one line and one device run. But it should be
tried expecting it to fail, not as the answer.

The other objection — `sdp.c`'s own warning that xCloud pattern-matches the
video m-line against per-device codec templates, and a half-match gets the
video track rejected outright — is the weaker one. The 1080p path already
varies both numbers and still matches, so the numbers are not what the match
keys on; the surrounding attribute shape is.

Separately and regardless of any of that: **the 720p offer should probably say
`max-mbps=216000`**, to describe the stream we are actually asking for and to
stop contradicting the 1080p path. Nothing observed suggests it is causing harm
today.

### Now unblocked: the device fingerprint

`device_info_header()` in `src/gnx/session.cpp` builds the `X-MS-Device-Info`
header at session creation — OS name, browser, and `displayInfo.dimensions` —
and it is what selects 720p over 1080p today. KNOWN-ISSUES §1 lists it as "the
one lever left to try" and as blocked on needing a patch to the green-nx clone.

It is not blocked any more. It is `src/gnx/session.cpp`, a file we own. Worth
noting the existing comment's caution when changing it: green-nx deliberately
reports the tier's real resolution because a fabricated one "risks landing in
an unknown-device profile server-side".

### Checked and ruled out

Negative results worth recording so nobody re-runs them:

- **DDR frequency scaling.** `/sys/class/devfreq/dmc` is already on the
  `performance` governor at its maximum 1056 MHz (of 324/528/780/1056). For a
  pipeline this memory-bound that looked like an easy win. It is already taken.
- **CPU governor.** One `policy0` covering all four A55s, `performance`, pinned
  at 1.8 GHz. Already optimal.
- **GPU.** `fde60000.gpu` idles at its 200 MHz minimum on `simple_ondemand`.
  Correct — nothing here touches the Mali, and leaving it down is free thermal
  headroom.
- **`netdev_max_backlog` = 1000.** Not a bottleneck at ~1500 packets/s.
- **Decoder flags.** `src/media/decoder.c` already rejects
  `AV_CODEC_FLAG2_FAST` with the right reasoning (on H.264 its only effect is
  non-conformant deblocking, which drifts from the encoder's reconstruction
  until the next IDR). The same argument rules out `skip_loop_filter`, which
  would otherwise be tempting given we discard half the pixels in the
  downscale anyway. Do not.
- **The NEON kernels** in `src/video/nv12.c` use the right idioms — `vst2q_u8`
  for the chroma interleave, `vld2q_u8` for the 2:1 box downscale. Nothing to
  gain there.
- **RGA**, the 2D block — already analysed and correctly rejected in
  the M2 display spec (project notes) §5. Nothing found here changes it.

- **`memset` of the 1300-byte agent buffer on every `peer_connection_loop()`
  call.** Dead work — `recvfrom` overwrites it and only `agent_ret` bytes are
  ever read. At roughly 1500 calls/s it is about 2 MB/s of pointless zeroing,
  call it 0.05 % of one core. Real but not worth claiming as an optimisation;
  noted only so the next person does not have to work out whether it matters.

### Latent, not currently biting

Small things found while reading. None is causing a live problem; all four are
the kind that surface later as something inexplicable.

- **`Http` never calls `curl_global_init()`.** `curl_easy_init()` does a lazy
  implicit global init, which libcurl documents as *not* thread-safe. It works
  today only because the first `Http` is constructed on the main thread before
  any worker exists. One call in `main()` removes the dependency on that
  ordering.
- **`Http`'s 15 s timeout is `CURLOPT_TIMEOUT`** — whole-transfer, not idle.
  The library screen fetches roughly 30 responses of about 1 MB; on a weak link
  that is a real ceiling, and it fails rather than slows.
  `CURLOPT_LOW_SPEED_LIMIT`/`_TIME` says "stalled" properly.
- **`decode_teredo()` requires the fully expanded 8-group IPv6 form** and drops
  the candidate otherwise. Not seen compressed in practice; not guarded either.
- **`rtp_decoder_init()` has no `break` after `case CODEC_OPUS`** in
  `third_party/libpeer/src/rtp.c`. Harmless today because it falls into
  `default: break;`, but it is one added case away from being a bug.

### On quality

The premise going in was that this might be amateurish borrowed code taken at
face value. It mostly is not. green-nx's core is careful, the comments explain
*why* rather than *what*, and several of them record real debugging (the
`errorDetails`-with-all-null-fields quirk on home consoles, the
`useIceConnection` flag that makes the console agent reject a start). libpeer
upstream is genuinely embedded-oriented and its RTP assembly was unusable here,
but our patch had already found and replaced that.

The thing actually worth taking at less than face value was not code quality.
It was a **request to the kernel that returned success without doing what it
said** — and the reason it survived so long is that nothing ever read back the
result. That is the pattern to watch for, and it is the same lesson as the
README's "a silent fallback is worse than a failure", one layer down.

The hardware-decode dig is separate and is in [HARDWARE-DECODE.md](HARDWARE-DECODE.md).

## Housekeeping noticed, not actioned

- **The project is not under version control.** `git init` here would be worth
  more than any single change in this document.
- `deps/src/libpeer` (about 70 MB) is the old clone. Nothing references it any
  more; it can go.
- `src/app/main.cpp.orig` is a leftover and still `#include`s the old
  `reference/green-nx/...` paths.
- `src/video/.idea/` is an IDE directory inside the source tree.
- A container named `xcloud-view` from an earlier session was still running
  `out/host/xcloud -nort` (idle, 0.01 % CPU). `docker rm -f xcloud-view` if it
  is not wanted.
