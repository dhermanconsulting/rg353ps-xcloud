# What actually controls the stream resolution

Measured on a live account through the host simulator, 2026-09-05. Roughly
twenty live xCloud sessions, one variable at a time.

**This supersedes the earlier "the device fingerprint picks the resolution"
finding, which was wrong.** It was wrong because `-tier` moved five signals at
once and the change was attributed to the only one that had been talked about.

## The model

```
resolution = min(resolutionAlias tier, the client's declared maximum)
bitrate    = f(resolutionAlias tier)          -- and only weakly f(declared cap)
```

The **`resolutionAlias`** is the lever. It rides on the control channel as
`userRequestedResolutionUpdate` ([xcloud_protocol.cpp](../src/gnx/xcloud_protocol.cpp)),
and the enum is `Auto / 720 / 720HQ / 1080 / 1080HQ / 1440` — the "user
selected resolution" feature Microsoft shipped in November 2025, found in the
xbox.com bundle by the Better xCloud developer.

The **device fingerprint is close to irrelevant.** This is the measurement
that settles it: a session claiming to be a Samsung TV (`tizen`), declaring a
1920x1080 display, sending 1080p capability messages and 1080p SDP decode
caps, but asking for alias `720`, gets **1280x720**.

| flags | encoded | bitrate |
|---|---|---|
| `-tier 720` | 1280x720 | 4.1 - 4.9 Mbps |
| `-tier 720 -alias 720HQ` | 1280x720 | 6.3 - 6.7 Mbps |
| `-tier 720 -alias 1440` | 1280x720 | 6.3 Mbps |
| `-tier 720 -alias 1080 -res 1920x1080` | **1920x1080** | 6.9 Mbps |
| `-tier 1080hq -alias 720` | **1280x720** | - |
| `-tier 1080hq` (alias 1080HQ) | 2560x1440 -> 1920x1080 | - |

Read the third and fourth rows together: alias `1440` on its own did **not**
raise the resolution, because the tier's capability messages still declared a
1280x720 maximum. Raising that ceiling with `-res 1920x1080` let alias `1080`
through. So the declared maximum is a genuine ceiling — it just cannot be used
as a floor.

## Sub-720p: the client cannot force it, the title can grant it

**Answer: xCloud does go below 720p — but only when the game asks it to, and
nothing the client does can compel that.** Measured at every layer a client
can reach. Each row is the strongest form of that lever that exists:

| Layer | What was tried | Result |
|---|---|---|
| Resolution alias | `-alias 540 / 480 / 360` | 1280x720. Unknown aliases are silently ignored, not rejected |
| Resolution alias | `-alias Auto` | 1280x720 (at 6866 kbps) |
| Capability messages | `maxWidth`/`maxHeight`/`maxPixels`/`preferredWidth` = 640x360 | 1280x720 |
| Session fingerprint | `-display 640x360`, `-display 640x480` (4:3) | 1280x720 |
| **SDP decode capability** | `profile-level-id=42e015` (level 2.1), `max-fs=792`, `max-mbps=19800` | **1280x720** |
| All of the above at once | consistent 640x360 everywhere | 1280x720 |
| **The title** | Fortnite, declaring 640x360 | **640x360 native** |

The SDP row is the one that settles the protocol question. Level 2.1 permits
792 macroblocks per frame; 1280x720 is 3600. The server **echoed our level
back in its answer** —

```
offer:  a=fmtp:102 ...;profile-level-id=42e015;max-fs=792;max-mbps=19800
answer: a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e015
```

— and then sent 1280x720 anyway, 4.5x the frame size it had just agreed to.
The stream did not fail or degrade; it simply ignored the negotiation. So the
encoder configuration is fixed server-side and the H.264 capability exchange
is decorative. There is no client-side lever left to try.

### The title-side route does work

Fortnite, given `maxWidth`/`preferredWidth` of 640x360:

```
xcloud: stream is now 1280x720
xcloud: stream is now 640x360
drm: source now 640x360 -> 640x360 at (0,60)
```

Native 640x360, held for the rest of the session, 60 fps with no dropped
frames, 1.0-1.8 Mbps, and 3-11% CPU against 30%+ at 720p. **No downscale at
all** — the plane is fed exactly the rectangle the panel shows, so VOP2's
scaler never runs and the coarse-text problem does not exist for that title.

Asking Fortnite for 640x480 still returns 640x360: the title clamps to 16:9.
It chooses, we only inform.

**Adoption is very rare.** Swept with `scripts/ressweep.sh`, 2026-09-06: of
**27 titles, exactly one** honours it.

| | Titles |
|---|---|
| **Native 640x360** | **Fortnite** |
| Fixed 1280x720 | Wreckfest, Halo Infinite, Forza Horizon 5, Gears 5, Sea of Thieves, Starfield, Doom Eternal, Diablo II Resurrected, Grid Legends, Minecraft Dungeons, Dead Cells, Hollow Knight Silksong, Hades, Balatro, Vampire Survivors, Terraria, Stardew Valley, PowerWash Simulator 2, Among Us, Asphalt 9, Brawlhalla, The Finals, War Thunder, Smite 2, Albion Online, Enlisted |

The sample was chosen to give adoption every chance: mobile-first games (Among
Us, Asphalt 9), free-to-play games with phone versions (The Finals, War
Thunder, Smite 2, Albion Online, Enlisted), and 2D indies whose render targets
are tiny anyway (Balatro, Vampire Survivors, Terraria, Stardew Valley). None
of it predicts adoption. Fortnite is simply an outlier — Epic ships it to
phones through cloud streaming and has the variable render target to match.

So in practice: **assume 720p, and treat Fortnite as the exception.** Sweeping
the remaining ~560 playable titles would cost a few hours of cloud time and is
the obvious way to find any others.

### A side observation on bitrate

The sweep's bitrate column varies far more by content than by anything the
client does — 75 kbps for The Finals against 5337 kbps for Balatro. Treat the
low numbers with suspicion: a 35 second session often does not get past a
loading screen on a large title, and a static screen encodes to nearly
nothing. The useful half of the observation is the top end, which is where the
Wi-Fi link (KNOWN-ISSUES 0) actually gets stressed: a busy 720p scene wants
4-5 Mbps and a quiet one wants a tenth of that.

### Why the capability route needs the title

Microsoft's own documentation says so.
[XGameStreamingSetResolution](https://learn.microsoft.com/en-us/gaming/gdk/_content/gc/system/overviews/game-streaming/game-streaming-custom-resolution-overview)
is a **title-side** API:

> `XGameStreamingGetDisplayDetails` returns display details of the specified
> client. ... `XGameStreamingSetResolution` should be used once the title has
> decided on a resolution. This API will set the resolution of the stream.

The client sends its display details — `preferredWidth`, `preferredHeight`,
`maxWidth`, `maxHeight`, `maxPixels`, exactly the fields in our
`dimensionschanged` and `clientdevicecapabilities` messages — and the **game**
then decides whether to act on them. If a title has not adopted the API,
nothing the client sends can change the render resolution. That is why `-res
640x360` is ignored: not because some other signal contradicts it, but because
nothing at the far end is listening.

So `supportsCustomResolution: true` is an honest statement of what our client
can accept. It is not a request, and it was never going to be honoured by a
title that does not call the API.

`maxPixels` — named in the GDK's `XGameStreamingDisplayDetails` alongside
`maxWidth`/`maxHeight` — was missing from our capability message until
2026-09-05. It is sent now. It changed nothing on its own, but it closes off
"we never even told it" as an explanation.

### What this changed in the client

We now declare **640x360** by default rather than the tier's 1280x720: it is
the rectangle this panel actually shows, so it is simply the truth. Measured,
it costs nothing on the nine titles that ignore it (same 720p, same bitrate —
4941 kbps against a 4104-4945 baseline spread) and it is what makes Fortnite
hand back a native encode. "Tell xCloud we are" in the options menu, and
`-res`; the default is set by absence of the key in `options.json`, so a saved
file that meant "as sent" still means it.

## What this is worth to us: 720HQ

The coarse-text problem (KNOWN-ISSUES 1) wanted a *smaller* encode, to avoid
VOP2's 2:1 downscale. That is unreachable. But the same problem has a second
cause — "a starved encoder smears small glyphs before any scaler sees them" —
and that one **is** reachable:

**`720HQ` is the same 1280x720 at roughly half again the bitrate.** Measured
back to back on the same title: 4542 kbps at `720`, 7154 kbps at `720HQ`. The
decoder does not care, because the frame size is unchanged; the extra bits go
straight into the fine detail that the downscale then has to work with.

It is `-tier 720hq`, and "Stream quality: 720p high" in the options menu.
Raising the declared cap as well adds a little more (6.9 Mbps at
`-bitrate 15000`), which is why `P720HQ`'s profile declares 15000.

**It is not the default, deliberately.** KNOWN-ISSUES 0 has the Wi-Fi link as
the remaining bottleneck at -63 to -67 dBm, and this asks about 50% more of
it. Whether the handheld's link sustains ~7 Mbps is an open question that
needs the device; on a weak signal it may trade sharper glyphs for dropped
frames, which is a bad bargain. Try it, watch `net|` and the drop counters.

## Method notes

- `scripts/resmatrix.sh` runs one live session per flag set and prints the
  encoded size and delivered bitrate. That is the instrument; the flags
  `-alias`, `-osname` and `-display` exist to move one signal at a time.
- The `net| video N kbps` line was added for this and is worth keeping: it is
  the only way to tell two streams of the same resolution apart.
- Bitrate varies with scene content, so single runs are not comparable to two
  significant figures. `720` measured 4104, 4542 and 4945 kbps across three
  runs of the same title. The `720` / `720HQ` separation is far larger than
  that spread, which is why it is reported as a finding and the smaller
  differences are not.
- ICE through Docker's NAT fails perhaps one run in four, usually as
  `binding request timeout`. A failed run still reports the size it saw
  before dying, so the table survives it, but never conclude from one run.

## Still open

1. **Does the handheld's Wi-Fi sustain 720HQ?** The whole value of that
   finding rests on it and it cannot be answered off-device. Note the
   opposite problem does not arise for an adopting title: Fortnite at 640x360
   asks for 1.0-1.8 Mbps, a third of what 720p needs.
2. **Sweep the rest of the catalogue.** 27 of 585 playable titles were
   tested and exactly one adopts. `scripts/ressweep.sh -f <list>` does the
   rest unattended at roughly 45 seconds a title, so the whole catalogue is
   a few hours of cloud time. Worth it: on this device an adopting title is
   categorically better, so the list is effectively "what to play on the
   handheld".
3. **Does an adopting title accept a size we prefer over its own choice?**
   Fortnite clamped 640x480 to 640x360, i.e. it treats our numbers as a hint
   and keeps its own aspect. Whether any title accepts 4:3 is unknown.
