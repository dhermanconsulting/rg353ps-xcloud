# Where the time actually goes

Performance analysis, 2026-09-06, overnight, with the handheld out of reach.

**Read this first: the host simulator cannot measure this device's CPU.** It
decodes a 720p frame in 0.11 ms where the handheld takes 10.7 ms — roughly a
hundred times faster, on a different SIMD unit. Any decode timing taken here
is meaningless for the RK3566, and one experiment below was abandoned for
exactly that reason. What the simulator *can* answer is anything about the
server's behaviour, the protocol, and the shape of the pipeline's logic. Those
are what this covers.

## Decode is not the bottleneck, and the obvious optimisation is a trap

The tempting idea: H.264's in-loop deblocking filter is a large slice of
decode time, this panel halves the picture before anyone sees it, so skipping
the filter should be nearly free.

It is a bad trade, and the reason is in [DEVICE.md](DEVICE.md):
software decode at 720p60 already runs at **2.54x realtime**, 7.24 s of CPU
per 5 s of video, about 36% of the SoC. There is no decode shortage to solve.
Spending picture quality — on a device whose single loudest complaint is text
legibility (KNOWN-ISSUES 1) — to buy headroom that is already there is
straightforwardly the wrong direction.

The knobs exist anyway, because "we think it would not help" is worth less
than a measurement someone can take in ten seconds when they next have the
handheld: `-skiploop none|nonref|nonkey|all` and `-lowres 1|2`. The decoder
reports the codec's `max_lowres` at open, so the lowres question answers
itself on first run (upstream removed lowres from the h264 decoder years ago,
so expect 0). **Neither is recommended.** If a future stream is genuinely
decode-bound, start here; otherwise leave them alone.

## The loss path: measured for the first time

This is the one that matters, because KNOWN-ISSUES 0 has the Wi-Fi link as the
remaining bottleneck and every unrecoverable loss goes through here.

The policy, in [video_jitter.cpp](../src/media/video_jitter.cpp): when a frame
cannot be assembled and its retransmit never came, drop it, set
`waiting_keyframe_`, ask for a PLI, and **discard every frame until a real IDR
arrives** — never feed the decoder a broken reference. So the cost of a loss
is exactly the keyframe round trip, and nobody had ever measured it.

`-plitest <s>` asks for a keyframe on a timer and the engine logs
`pli->idr N ms`. Live sessions, 2026-09-06:

| Stream | n | mean | median | range |
|---|---|---|---|---|
| Wreckfest, 1280x720 | 12 | **101 ms** | 91 ms | 56 - 217 ms |
| Fortnite, 640x360 | 7 | **69 ms** | 62 ms | 47 - 132 ms |

The first request of a session is excluded: it measured 1155 ms, which is
stream bring-up rather than steady state. The 217 ms outlier came from a
second sample taken later; a first, tighter sample of 8 suggested 68-115 ms,
so treat the spread as real and the median as the number to reason with.

**So an unrecoverable loss costs about a tenth of a second of frozen picture,
not seconds.** That vindicates the "never decode a broken reference" policy —
~100 ms of freeze is a far better bargain than seconds of smeared macroblocks
drifting until the next natural IDR. It also means keyframe recovery is *not*
the explanation for KNOWN-ISSUES 0's "stutters every 10-20 s"; that has to be
the raw loss and the multi-second gaps themselves.

A smaller stream recovers faster, as you would expect from a smaller IDR:
360p is about 25% quicker than 720p.

### Impaired replay: which fixture you use decides the answer

**A recording cannot answer a PLI.** In replay the client asks for a keyframe
and nothing comes until the recording's own next IDR, so how badly a loss
hurts is decided entirely by how densely that fixture carries IDRs. Same
impairment (`jitter=5,loss=0.5,burst=3`), same number of losses, two fixtures:

| fixture | lost | discarded | fps |
|---|---|---|---|
| `synth-20s.xcau` (keyint 60, one IDR/s) | 9 | **37** | 57.5 |
| `rec-wreckfest.xcau` (a real capture) | 9 | **671** | 26.1 |

Nine lost units cost 37 frames on one and 671 on the other. Nothing about the
client differs between those runs.

**`scripts/test.sh` is fine**, and an earlier revision of this file wrongly
implied otherwise: its `impaired` check uses the synthetic fixture, which
`tools/mkstream.cpp` builds with `keyint 60` by default — a comment there
records that a sparser one "turned a 1 % loss run into a black screen". The
misleading numbers come from impairing a *real capture*, which is what the
table above and an earlier draft of this document did.

So: **for loss and recovery work use the synthetic fixture, or a capture made
with `-plitest` to force dense IDRs.** Real captures are the right fixture for
pacing and decode work and the wrong one for loss.

One loose end, not yet measured: live pace lines show 6 IDRs in 12-21 s, i.e.
one every 2-3.5 s, which does not obviously explain a real capture discarding
671 frames (~11 s) after a single loss. Either `rec-wreckfest.xcau` is sparser
than those runs or something else is going on. Counting the IDRs in that
fixture directly is the next step and needs nothing but the simulator.

## The 640x360 path is better on every axis at once

Since [RESOLUTION.md](RESOLUTION.md) found that titles which adopt
`XGameStreamingSetResolution` encode at whatever we declare, it is worth
stating what that is worth in performance terms rather than only in sharpness:

| | 1280x720 | 640x360 | ratio |
|---|---|---|---|
| Macroblocks per frame | 3600 | 920 | **3.9x fewer** |
| Delivered bitrate (measured) | 4.1 - 4.9 Mbps | 1.0 - 1.8 Mbps | **~3x less** |
| PLI recovery (measured, median) | 91 ms | 62 ms | 1.5x faster |
| Downscale | 2:1 on VOP2 | **none** | - |
| Host CPU during replay | 30%+ | 3 - 11% | - |

Decode cost is roughly linear in macroblocks, so the expected device decode is
about **2.7 ms against 10.7 ms** — from 36% of the SoC to under 10%. That is
not measured on the handheld and should be checked there, but the macroblock
count is not in doubt.

The bitrate point is the important one for KNOWN-ISSUES 0: a third of the
bitrate on a link that is already the bottleneck. An adopting title is not
marginally better on this device, it is a different class of experience.

## What the real limits are, in order

1. **The Wi-Fi link.** Measured on-device: 25 dropped frames, 64 retransmit
   requests and gaps to 2 s across a five-minute race, at -63 to -67 dBm. Not
   improvable in software beyond what has been done (power save off, scan
   deny, re-NACK, bitrate adaptation). The lever that remains is *asking for
   less*: 360p where a title allows it, and not defaulting to 720HQ.
2. **The downscale**, for text (KNOWN-ISSUES 1). Unreachable except by
   getting a native 360p encode.
3. **Decode**, at 2.54x realtime. Comfortable, and the first thing that would
   bite if the stream ever went above 720p — which is why the 1080 tiers are
   selectable but not sensible here.
4. **RAM**, at 35-38 MB resident of 1 GB. Not a constraint.

## Open

1. **Model PLI response in the replay harness**, so impaired replays predict
   live behaviour instead of a worst case that cannot happen.
2. **Confirm 360p decode cost on the handheld.** The 3.9x macroblock ratio
   predicts ~2.7 ms; the device is the only place to check it.
3. **Measure PLI latency on the handheld's real Wi-Fi**, where the request or
   the answering IDR can itself be lost — the exponential PLI backoff in
   `request_keyframe_locked()` exists precisely for that case and has never
   been observed doing its job.
