# Remote play from your own Xbox (xHome)

Streaming your own console instead of a datacentre blade. Same client, same
transport, different GSSV offering.

**Why it matters:** from November 2026 cloud gaming is unbundled from Game
Pass and metered (Ultimate 15 h/month, Premium 10, Essential 5 —
see the note on commercial risk below). Remote play from a console
you own is **not** metered by those caps. On the LAN it should also negotiate
a direct ICE candidate pair to the console rather than routing to a
datacentre, which is a latency win over xCloud on a link that
[VIDEO-PACING.md](VIDEO-PACING.md) already identifies as the bottleneck.

## State, 2026-09-06: it works

Enabling remote features on the console makes it appear immediately:

```
xcloud: xhome host https://uks.core.gssv-play-prodxhome.xboxlive.com, 1 console(s)
console: <serverId>           (XboxOneX, ConnectedStandby)
```

**First live remote-play session the same day, and it is better than xCloud on
every measure that matters here:**

| | remote play | xCloud 720p |
|---|---|---|
| Frame rate | 60 fps, `held=0-1 skip=0 err=0 late=0` over 165 s | the same |
| Latency | **41-52 ms** | 55-75 ms |
| Bitrate (dashboard) | 26-104 kbps | 4.1-4.9 Mbps |
| Metered against Game Pass hours | **no** | yes |

The latency win is the point: a LAN-direct candidate pair instead of a round
trip to a datacentre. The bitrate figure is not comparable — a nearly static
dashboard encodes to almost nothing — but it does show the encoder responding
sensibly to content.

The H264 level munge was needed and accepted: the offer carried
`profile-level-id=42e020` and the console's answer echoed `42e020` back.

### The console has to be woken, and the first attempt is what wakes it

**A console in `ConnectedStandby` fails its first request, every time.**
Measured twice:

```
session state: provisioning
session state: failed
FAIL: Session failed: {"code":"AgentCommandError","message":"Agent :
  -2147467259 : class Network::StartStreamingSessionV2Command failed. at
  xbox\streaming\managementservice\xboxstreaminghelper.cpp:1426 :
  State ServerStartStreamingV2CommandSent"}
```

`-2147467259` is `0x80004005`, E_FAIL. The state reached
`ServerStartStreamingV2CommandSent`, so the service did deliver the start
command and the console's own agent failed it — and the console went from
`ConnectedStandby` to `On` in the process. A request made once it is awake
provisions in about a second.

So this is a wake, not a failure. The engine now treats it as one: on
`AgentCommandError` for a console target it polls `/v6/servers/home` until the
power state says `On` (capped at 30 s) and asks again, showing "Waking your
Xbox...". Without that every cold start looks broken and has to be retried by
hand.

**`serverName` came back empty** on this account, so the UI cannot rely on it.
`console_label()` falls back to the console type with spaces inserted, which
renders "XboxOneX" as "Xbox One X".

### The simulator is a poor place to test this

Two of four xHome sessions from the container failed, both in ICE. The reason
is visible in the candidates: the container is on Docker's `172.17.0.2` bridge
and the console is a LAN peer the container cannot route to, so the only
working path is a hairpin off the site's public address —

```
local  cand: 172.17.0.2 ... typ host
peer   cand: 203.0.113.7:3074             (public, hairpin)
peer   cand: 192.0.2.50:9002              (unroutable from the container)
```

(Addresses here and below are RFC 5737 documentation examples.)

— and it drops consent shortly after connecting. **The handheld is on the same
LAN as the console**, so it should get a direct pair with no hairpin at all,
and be both more reliable and lower latency than these numbers. That is the
first thing to confirm on the device.

## What was changed

Almost nothing, because [the `gnx` layer already implemented the whole
protocol](../src/gnx/session.cpp) — `start_home()`, `fetch_home_consoles()`,
the `xhome` login in [auth.cpp](../src/gnx/auth.cpp), the `"home"` platform
argument to `cleanup_stale_sessions()`, and the null-`errorDetails` quirk that
home consoles attach to perfectly good SDP responses. None of it was reachable
from the app.

- **[engine.hpp](../src/net/engine.hpp)** — `StreamTarget { Cloud, Console }`.
  `Engine::start()` takes it; `cloud_` became `creds_`, since it now holds
  whichever offering's credentials the target names.
- **[engine.cpp](../src/net/engine.cpp)** — the worker picks `creds.home` vs
  `creds.cloud`, passes `"home"` vs `"cloud"` to stale-session cleanup, and
  calls `start_home()` vs `start_cloud()`. An empty host on the console path
  reports `home_error` rather than the cloud path's generic message, because
  "no Xbox on this account" is a normal outcome and the only explanation
  anyone will get.
- **[session.cpp](../src/gnx/session.cpp)** — `sdp_set_h264_level_32()`, a
  same-length in-place bump of `profile-level-id` from `42e01f` (level 3.1) to
  `42e020` (level 3.2). The console's streaming agent is stricter than the
  cloud one, which accepts 3.1. Done as a string munge next to
  `sdp_force_stereo` and `sdp_scale_video_caps_1080` rather than by patching
  vendored libpeer, so `third_party/` stays pristine.
- **[stream.cpp](../src/app/stream.cpp)** — the session loop now takes a
  `StreamRequest` (target + id + display name) instead of a `gnx::Game`.
  `stream_game()` survives as a thin wrapper, so the library screen is
  untouched.
- **[main.cpp](../src/app/main.cpp)** — `-console <serverId>` and `-consoles`,
  mirroring `-title` and `-list`. Both run before the catalog is fetched:
  remote play streams the console's dashboard, not a title, so the library is
  not on the path at all.

The device fingerprint delta — xHome wants
`android` — costs nothing here: that is `QualityTier::P720`, which is already
the only tier this panel uses.

## Cloud regression, same build

55 s of Wreckfest over xCloud after the refactor, to confirm the shared path
still behaves:

```
pace| shown=61 held=0 skip=0 hold=61/0/0/0 q=2 auq=0 stale=0 | dec=61 0.2/0.5ms
  idr=0 slices=1-1 err=0 | ... late=0 lat=62/65ms | cpu=70% temp=43C rss=38MB
```

60-61 frames shown per second, no skips, no decode errors, no late commits,
55-75 ms end-to-end. Unchanged from [VIDEO-PACING.md](VIDEO-PACING.md).

## The bug that made it unusable: a starved encoder cannot answer a PLI

Remote play ran for 30 seconds on the handheld, lost one packet, and then sat
blind until the 15 s stall watchdog killed it — through nine PLIs, receiving
video the whole time. Three hypotheses died before the right one:

- *The console sends recovery I-frames we do not recognise.* No. Counting NAL
  types in a recorded console stream: 5 real IDRs (type 5) in 75 s.
- *The console ignores PLIs.* No. `-plitest` against it measured
  **59 ms**, faster than the cloud's 91 ms.
- *The receive buffer overflows on the IDR burst.* No. The device gets
  1024 KB via `SO_RCVBUFFORCE` (DEPENDENCIES.md); it is the *simulator* that
  is stuck at 208 KB.

What settled it was instrumenting the discard path to print the NAL types it
was throwing away. Every access unit arriving while blind contained **type 1
and nothing else** — pure P-frames, no IDR, no SPS/PPS. Meanwhile `remb=1200`
sat in every stats line.

**The device's `options.json` had `"bitrate":1`, which is the menu's 1200 kbps
cap** — left over from the encoder-starvation experiment in KNOWN-ISSUES 1.
That value goes into `clientdevicecapabilities.maxBitrateKbps` and into every
REMB we advertise. The console streams a static dashboard inside 1200 kbps
happily at ~50 kbps; what it cannot do is fit a 720p IDR in that budget. So
each PLI was answered with more P-frames, every one of which we discarded:

```
loss -> drop -> waiting_keyframe_ -> PLI -> encoder cannot afford an IDR
     -> P-frames -> discard -> PLI -> ... forever
```

Clearing the cap fixed it outright: 5996 frames over 100 s, `resync` stopped
at 54, no stall.

**But a menu option should not be able to deadlock the stream**, so the engine
now refuses to advertise a rate that cannot carry a keyframe *while it is
asking for one*: if `waiting_keyframe()` and the advertised REMB is below
4000 kbps, it advertises 4000 until a frame arrives, and says so in the log.
The user's cap is honoured the rest of the time — the alternative to briefly
ignoring it is a stream that never comes back.

Verified with the 1200 kbps cap deliberately left in place: three stalls,
three recoveries, `resync` held at 1 where it had climbed to 585 before.

Then at default settings, a clean 150 s run: **8143 frames across 137 s of
presenting, sustained 60 fps, one dropped packet recovered, no failure.**

## The UI

Built 2026-09-06. Remote play is a **"Remote play" section at the top of the
library**, above Recently played, one row per console — not a separate screen
and not a picker. The reasons: there is no title to choose (remote play
streams the dashboard), the list is one item on a normal account, and a picker
for a list of one is a button press wasted. Selecting a row goes straight to
`stream_session()` with `StreamTarget::Console`.

Each row shows the console's name (or its model, see above) and its power
state as "Ready" or "Asleep, will wake" — the latter because from standby the
first connection has to wake it and takes appreciably longer, and a progress
message beats a mystery.

`-fake-consoles N` synthesises the rows with no account and no network, which
is how the screen was built and how `scripts/test.sh`'s UI check can cover it.
The first fake console deliberately has no name, because that is the case the
real account turned up.

## Open, in order

1. **One session ended with "Connection timed out" and is not explained.**
   During the fix verification a first session recovered and streamed, a
   second was created, connected, and then timed out 45 s in. Seen once,
   distinct from the starvation stall above, and not chased. Watch for it.
2. **Confirm the wake retry end to end.** The code polls for `On` and asks
   again, but it has only been exercised against a console that was already
   awake — the cold path was measured before the retry existed. Put the Xbox
   in standby and start a session.
3. **Does the dashboard need a different input mapping?** Remote play streams
   the console UI, where the Xbox button and the view/menu buttons matter more
   than in a game. Nexus is wired; whether it behaves is untested.
4. **Audio.** Never checked on this path at all.
