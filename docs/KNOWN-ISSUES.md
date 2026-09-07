# Known issues

Status as of 2026-09-07.
The pacing work is described in [VIDEO-PACING.md](VIDEO-PACING.md).

## Fixed since the last revision

- **Playback judder (was issue 1).** Root cause measured, not guessed: the
  stream is single-slice so decode ran on one thread, and the one-thread
  loop threw away 30-70 % of decoded frames whenever two units were queued.
  Now a frame-threaded decode thread feeds a vsync-paced presenter. Live:
  a new frame on every refresh of 60 fps content, `late=1` in 102 s, zero
  queue overflows. Details and numbers in VIDEO-PACING.md.
- **Audio stutter and latency (was issue 2).** The blocking whole-batch
  write and the encoded-packet shedding are gone; the codec is driven
  directly at 48 kHz with a sample-level drift servo. Live: 29-53 ms
  queued, `lost=0`, one underrun at start.
- **The library screen (was issues 5 and 6).** Rewritten 2026-09-05
  (src/app/library.cpp, library_data.cpp, thumbs.cpp): every title's name
  resolved in the background and cached (names.json), the catalog cached
  (catalog.json) so the list is instant on the next launch, sorted by name
  with the letter jump on names, recently played at the top, box-art
  thumbnails (80x120, cached on disk, 10 MB in memory), a letter-picker
  search on Y, and UTF-8 text with a glyph cache. Repaints measure
  0.2-0.5 ms on the host. The real-account path is exercised only as far as
  the simulator can go without tokens; the first launch on the device is
  the test.
- **Ghosting / visual defects mid-stream** and **library scrolling**, from
  the previous revision, remain fixed.

---

## 0b. A low bitrate cap deadlocks recovery -- fixed 2026-09-07

Setting the menu's "Bitrate cap" low enough (1200 kbps was found on the
device) makes any single packet loss permanent: the client asks for a
keyframe, the encoder cannot fit an IDR inside the advertised rate, answers
with more P-frames, and every one is discarded while waiting. The stream never
comes back and the 15 s stall watchdog kills it.

The engine now lifts the advertised REMB to 4000 kbps whenever it is blind and
asking for a keyframe, restoring the cap once a frame arrives -- advertising a
rate that cannot carry the frame you are begging for is self-defeating. Full
diagnosis, including the three hypotheses that were wrong first, in
[REMOTE-PLAY.md](REMOTE-PLAY.md).

Found on remote play, but nothing about it is remote-play specific: the same
deadlock is available on the cloud path to anyone who sets the cap low on a
lossy link.

## 0. Live play stutters every 10-20 s and hangs under load: the Wi-Fi link

Measured from a five-minute race (docs/VIDEO-PACING.md, "Live play"): 25
dropped frames, 64 retransmit requests, gaps up to 2 s, 250 lost audio
packets, with the client's own timing clean throughout. Causes on the
device: the Wi-Fi driver in maximum leisure power save, connman's 30 s
background scan at this signal level, and a weak signal. It is not the
band: the chip is dual-band and is already on the 5 GHz SSID (channel 64,
80 MHz operating width), but at -63 to -67 dBm the driver transmits at
20 MHz. The client now turns power save off (nl80211) and refuses scans at
the driver (scan_deny) for the stream, verified 2026-09-05: the driver's
power-save entry counter stayed at 38 across a 90 s stream that logged
3944 frames, drop=0, nack=0, audio lost=0. It also recovers from loss
faster and adapts its bitrate; the rest is signal strength, i.e. the
distance to the access point. wpa_supplicant runs D-Bus-only on this
firmware (no control socket), so the wpa_cli route in the first version
of this could never have worked here.

## 1. Text is coarse on the panel; the hardware scaler is why

Reported 2026-09-05: games look great, text does not. Root cause measured,
not guessed. Nothing in the client scaled anything: the plane took a full
1280x720 buffer with a 640x360 destination and VOP2's Esmart scaler halved
it. Horizontally that is a 2-tap bilinear stepping 2.00146 source pixels
from phase 0, so over most of the line one column is taken and its
neighbour discarded, and the surviving parity drifts across the picture;
vertically the GT2 pre-scaler does the whole job and the vertical filter is
bypassed. Photographic content does not miss the discarded half. Glyph
stems are 1-2 pixels at 720p, so they survive, vanish or double by parity,
and the parity changes as the picture pans.

`-scale box|sharp` halves the frame on the CPU instead and hands the plane
a source it need not scale; `-res <w>x<h>` asks xCloud to encode smaller in
the first place, which would avoid the downscale altogether. Both are in
docs/VIDEO-PACING.md, "The downscale", with the arithmetic, the error
table and `tools/scaletest.c`, which shows the difference without a device.

Measured on the device at a live 60 fps: `cvt` is 0.7 ms with `hw`, 1.1 ms
with `box` and 1.9 ms with `sharp`, against a 16.58 ms budget, with
`late=0` and no dropped frames in any of the three. All three can be
switched from the options menu (SELECT + X) while a stream is running.

**Encoding smaller at the source is not available.** Two ways of asking
have been tested on the device and both are ignored: the capability
messages (`-res 640x360` still returns 1280x720) and starving the encoder
(110 s at an advertised 1200 kbps, still 1280x720 at 60 fps -- xCloud
spends a shortfall on quality, not on pixels). The one lever left is the
device fingerprint at session-creation time, which is what selects 720p
over 1080p today; it is written up in VIDEO-PACING.md, "Then why not, and what
is left to try".

**That lever is no longer blocked.** It said "it needs a patch to the
green-nx clone"; since 2026-09-05 that code is ours, at
`src/gnx/session.cpp`, `device_info_header()`. Heed the existing comment
there when changing it — green-nx reports the tier's real resolution on
purpose, because a fabricated one risks landing in an unknown-device
profile server-side.

**And it no longer needs a rebuild to try.** The tier is the "Stream quality"
row in the options menu, and `-tier 720|1080|1080hq` on the command line.

**The fingerprint is a red herring, and an earlier revision of this file said
otherwise. It was wrong.** `-tier` moved five signals together — the session
fingerprint, the claimed display size, the control channel's
`resolutionAlias`, the capability messages and the SDP decode caps — and the
resolution change was pinned on the only one that had been discussed.

Decomposed over ~20 live sessions, 2026-09-05, one variable at a time
([RESOLUTION.md](RESOLUTION.md) has the full table and method):

```
resolution = min(resolutionAlias, the client's declared maximum)
bitrate    = f(resolutionAlias)
```

A `tizen` session declaring a 1920x1080 display, 1080p capabilities and 1080p
SDP caps, but asking for alias `720`, gets **1280x720**. The alias is the
lever; the fingerprint barely matters.

**720p is a floor the client cannot get under — but the title can.** Every
client-side lever was tried and every one failed: `-alias 540/480/360` and
`Auto` all return 1280x720; the capability messages, `maxPixels` included, do
nothing on their own; the session-creation display size does nothing at
640x360 or at 640x480; and — the one that settles it — declaring H.264
**level 2.1** with `max-fs=792` in the SDP got our level *echoed back in the
server's answer* and then 1280x720 regardless, 4.5x the frame size it had
just agreed to. The capability negotiation is decorative.

What does work is the game.
[Custom resolution is a title-side API](https://learn.microsoft.com/en-us/gaming/gdk/_content/gc/system/overviews/game-streaming/game-streaming-custom-resolution-overview):
the client sends `preferredWidth`/`maxWidth` and the **title** decides whether
to call `XGameStreamingSetResolution`. **Fortnite does** — it drops to
**640x360 native**, which is exactly the rectangle the panel shows, so VOP2's
scaler never runs and this issue simply does not exist for that title (also
60 fps, 1.0-1.8 Mbps, 3-11% CPU). Nine other titles tested ignore it,
including mobile-first ones like Among Us and Asphalt 9.

The client therefore now declares 640x360 by default: it is the truth about
this panel, it costs nothing where it is ignored, and it is what makes an
adopting title hand back a native encode.

**What did come out of it: `720HQ`.** The same 1280x720 at about half again
the bitrate — 4542 kbps against 7154 kbps, measured back to back. This issue
has two causes, and while the downscale is unreachable, "a starved encoder
smears small glyphs before any scaler sees them" is not. The extra bits land
in exactly the detail the 2:1 halving then has to work with, and the decoder
does not notice because the frame size is unchanged. It is `-tier 720hq`, and
"Stream quality: 720p high" in the menu.

**Not the default, pending the device.** It asks ~50% more of a Wi-Fi link
that issue 0 already calls the bottleneck. Whether the handheld sustains
~7 Mbps at -63 to -67 dBm is the open question; watch the new `net|` line and
the drop counters. On a weak signal this could trade sharper glyphs for
dropped frames.

So the levers now are: **720HQ** for encoder quality, and `-scale box|sharp`
for the downscale itself. Both are in the options menu; `box` against `sharp`
still needs eyes on the panel.

A second, cheaper thing to try first is the SDP offer's declared decode
limit (`max-fs` in `third_party/libpeer/src/sdp.c`), though there is
already evidence against it: the same line declares `max-mbps=108000`,
which is 720p**30**, and we receive 720p60 regardless. See
docs/DEPENDENCIES.md, "Untried lever on the coarse-text problem".

**Still open only because the default is unchanged (`hw`) pending a look at
the panel**: box against sharp is a judgement on a 3.5" screen, not
something the error table decides.

## 1b. The SD-card install: verified, with one gap

Confirmed on an RG353PS on 2026-09-07:

- **The ROM card runs binaries.** It is exFAT through FUSE, mounted at
  `/userdata/roms` with `rw,relatime,default_permissions,allow_other` and
  **no `noexec`**; files present as `0777`. The client runs from the card.
- **The launcher finds the client beside itself** and starts it.
- **First-run gamelist registration is safe on real data.** Against a device
  gamelist holding 48 entries: 48 afterwards, ours replaced rather than
  duplicated, no temporary left behind.
- **The token store survives on both filesystems.** `xcloud -selftest` reports
  save, reload, replace and `0600` on `/userdata` (ext4); on the card it
  passes with "permissions not restricted", which is exFAT having no
  permission bits, not a failure.
- **A stream runs from the card binary** at a sustained 60 fps -- `shown=61
  held=0 skip=0`, `drop=0 nack=0 resync=0`, 74-80% CPU, 40-42 C.

**The remaining gap:** nobody has yet done the *whole* cold path -- copy the
folder onto a card on a computer, put the card in, boot, and see the entry
appear -- so whether EmulationStation lists a newly copied port without a
restart is still unconfirmed. [INSTALL.md](INSTALL.md) tells people to reboot
anyway, so the instructions are correct either way; what is unknown is only
whether that step is necessary.

## 2. Resolution change blanked the plane — fixed 2026-09-06, unverified

`drm_out_set_source` freed the framebuffer that was scanning out, which the
driver turns into a synchronous plane disable: one black refresh and a
20-40 ms stall each time the resolution changes.

**This got worse before it got fixed.** It was filed as minor because
resolution changes were rare — a loading screen, a bandwidth step. Then the
client began declaring 640x360 by default (KNOWN-ISSUES 1), so any title that
honours it changes resolution on the way in, and the stall moved onto the best
path we have:

```
drm: source now 1280x720 -> 640x360
drm: source now 640x360  -> 640x360
```

Fixed as M5-PACING sketched: the old buffers are held, the new pair allocated,
one new buffer committed and its flip awaited, and only then are the old ones
released. `atomic_commit` already reprograms `src_w/src_h/crtc_*` on every
commit, so a single commit swaps framebuffer and geometry together and the
plane is never left with nothing to scan. Both pairs exist for one flip: a
transient 2.8 MB at 720p. If the new size cannot be allocated the old pair is
put back and kept on screen rather than tearing down a working plane.

**Verified only by construction and a clean build.** The simulator uses
`drm_output_sim.c`, which mallocs and has no plane to disable, so it cannot
show the artefact or the fix. Watch for the black frame on a Fortnite launch
when the handheld is next to hand.

## 3. Audio falls back to the dmix path if something holds the card

The direct 48 kHz device is exclusive. Verified from the Ports menu on
2026-09-05 (session log: "audio: plughw:0,0: buffer=9600 frames"), so
EmulationStation does not hold it while a port runs. If anything ever
does, the player logs "direct device busy" and uses `default` (44.1 kHz
resample, 23 ms quantisation, weaker drift control).

## 4. A lost audio packet is concealed, not perfect

A packet the reorder buffer gives up on (5 ms grace: audio has no
retransmission) is filled by libopus packet-loss concealment when
`libopus.so.0` is on the device (loaded at runtime; libavcodec there links
it, so it should be) and by silence otherwise; the log says which at
stream start. Concealment extrapolates the last frame, so a single loss is
near-inaudible and a burst fades. Not yet heard on the device.

## 5. Input works; the face-button layout is a guess until pressed

Buttons and sticks act in games. The face buttons now follow the printed
labels (A is the button that says A) on the strength of the device tree's
GPIO names and one earlier measurement; if X and Y feel swapped, `-layout
xbox` is the other mapping and the log line "pad: face buttons follow..."
says which is active.

**Both are in the options menu since 2026-09-05** ("Face buttons" and "Swap
X and Y", on SELECT+X mid-stream or START in the library), which is the point:
this is a judgement made with a thumb, and it used to be settable only from a
command line the handheld does not have. The change applies to the open pad
immediately and is saved, so the next launch keeps it. See
the M7 notes (project notes).

## 6. Search cannot type non-Latin letters

The picker offers A-Z and digits; titles with kana or Cyrillic names are
found by their Latin launch id instead. Glyphs the DejaVu face lacks draw
as `?`.

## 7. Menu: first launch after this revision resolves 585 names

About 30 requests of ~1 MB each, in the background with a progress bar in
the header; the list is usable meanwhile and complete from disk on every
launch after.

## 8. Minor: harmless libcurl warning

`/usr/lib64/libcurl.so.4: no version information available`, once per
launch. Cosmetic.
