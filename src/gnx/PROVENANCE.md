# src/gnx/ — provenance and licence

These ten files are the xCloud account, catalog and session layer. They came
from **green-nx**, `src/core/`:

- upstream: <https://github.com/rmrf404/green-nx>
- commit: `e498290dccbeef46154f985150e122b1e6e8965e` (2026-08-24)
- licence: **GPL-3.0** — the full text is in `LICENSE.GPL-3.0` beside this file

They are compiled into `out/xcloud`, so **the client as a whole is GPL-3.0**.
That was already true when they were compiled out of `reference/`; copying them
here does not change the obligation, it just puts the licence next to the code
it covers instead of three directories away.

## Why they are in `src/` and not `third_party/`

Because they are ours to change now. The build used to compile them straight
out of `reference/green-nx/`, a live clone of somebody else's repository — a
`git pull` there would silently have changed what the client does, and any fix
we made would have been a local modification to a checkout with nothing
recording that we had made it.

They are also code we have a concrete reason to modify. `session.cpp`'s
`device_info_header()` is the device fingerprint that decides whether xCloud
encodes at 720p or 1080p, and it is the last untried lever on the coarse-text
problem (`docs/KNOWN-ISSUES.md` §1). That is a change to make here, in a
file we own, not a patch carried against a clone.

## Changes from upstream

Rather more than the two the copy started with — this is what "ours to change
now" turned into. Grouped by file:

**Mechanical, applied to every file when they were copied on 2026-09-05**

- `#include "../../vendor/json.hpp"` became
  `#include "../../third_party/nlohmann/json.hpp"` in `auth.cpp`,
  `catalog.cpp`, `session.cpp` and `xcloud_protocol.cpp`. The header itself is
  byte-identical to the one green-nx vendored (nlohmann/json 3.11.3, MIT).
- Line endings normalised from CRLF to LF, matching the rest of `src/`. The
  `reference/` clone has CRLF because it was checked out on Windows.
- `LICENSE.GPL-3.0` and this file were added alongside the code. They are the
  licence and this note, not source; `diff -r` reports them as
  `Only in src/gnx:`.

**`auth.cpp`, `auth.hpp` — the token store**

- `save_refresh_token()` writes a temporary file and `rename(2)`s it over the
  target instead of truncating in place, and `chmod`s it to 0600 before the
  rename. The refresh token is a long-lived credential to the user's Microsoft
  account, so two things about the upstream version matter: it should not be
  left world-readable wherever `scripts/pull.sh` copies it off the device, and
  a truncating write that then fails — a full `/userdata` being the realistic
  case — leaves an empty file that reads back as "not signed in", silently
  costing the user their session. `rename(2)` within a directory is atomic, so
  the old token survives any failure before it. A failing `chmod` warns once on
  stderr and is otherwise ignored: the ROM card is exFAT and has no permission
  bits at all, and signing in again on every launch would be far worse than a
  file that cannot be mode-restricted on a single-user handheld. Brings
  `<sys/stat.h>`, `<sys/types.h>`, `<cerrno>` and `<cstring>` with it.
- `XboxAuth::selftest_store()` is new, and is what `xcloud -selftest`
  (`src/app/main.cpp`) runs. `save_refresh_token()` is otherwise reachable only
  from a live sign-in, so nothing offline covered the one path that decides
  whether a user stays signed in — and since it is filesystem behaviour, it has
  to be checked on the device rather than on a build host.

**`catalog.cpp`, `catalog.hpp` — genre, rating and year**

- `Game` gains `genre`, `rating`, `rating_count` and `year`, and `fetch_names()`
  fills them from `Properties.Category`, the widest `MarketProperties`
  `UsageData` window, and `OriginalReleaseDate`. All four already rode along in
  the response — we ask for `fieldsTemplate=Details` — and were being thrown
  away. The library screen's genre browsing and sorting (`src/app/library.cpp`)
  are built on them.

**`session.cpp`, `session.hpp` — the resolution work**

- `QualityTier` gains `P720HQ`, with `is_720_tier()` beside it for the two
  tiers that share the same 1280x720 encode. The header comment explaining the
  tiers was rewritten at the same time: the old one described the device
  fingerprint as the thing that picks resolution, which the 2026-09-05
  measurements in `docs/RESOLUTION.md` showed to be wrong. `P720HQ` is the
  useful tier on this panel — the same 720p at roughly half again the bitrate.
- `set_device_override()` and the `os_name_override_` / `display_w_override_` /
  `display_h_override_` members, threaded through `device_info_header()`. A
  research hook: a tier normally moves the fingerprint, the claimed display
  size and the resolution alias together, so without these no change in what
  the server encodes can be attributed to any one of them.
- `fetch_wait_time()` (`GET /v1/waittime/{titleId}`) is new, so a session
  sitting in `WaitingForResources` can be told apart from a protocol fault.
  The response shape is unverified — the endpoint is documented and nothing
  else — so it accepts the field names other clients use, gives up quietly
  otherwise, and hands back the raw body for the first live queue to pin down.
- `sdp_set_video_caps()` and `sdp_set_h264_level_32()` are new. They rewrite
  the offer's declared H.264 decode limits (`max-fs`, `max-mbps`,
  `profile-level-id`), which a conforming encoder may not exceed and which is
  therefore a much stronger statement than the application-level capability
  messages. Level 3.2 is xHome only: the console's own streaming agent is
  stricter than the cloud one, which accepts 3.1 happily.

**`xcloud_protocol.cpp` — `maxPixels`**

- `maxPixels` is now sent in both display-capability messages. The GDK names it
  alongside `maxWidth`/`maxHeight` in `XGameStreamingDisplayDetails` as a bound
  titles are asked to respect, and we were omitting it entirely; it went in on
  2026-09-05 to close off the "we never even told it" explanation while
  `docs/RESOLUTION.md` was being written.

That is the whole list — but **compare with `diff --strip-trailing-cr`** or the
line endings alone make every line look changed:

```bash
diff --strip-trailing-cr -r reference/green-nx/src/core src/gnx
```

`reference/` is gitignored, so this only works in a checkout that has green-nx
cloned there. It should show exactly the changes above and the two
`Only in src/gnx:` lines; anything more means this section has gone stale,
which it has done once already. `http.cpp`, `http.hpp` and
`xcloud_protocol.hpp` are still byte-identical to upstream apart from line
endings. Reading an upstream fix across is no longer the free operation it was
when this list was two bullets long — `auth.cpp` and `session.cpp` in
particular have diverged enough to need a real merge.

## What each file does

| File | Role |
|---|---|
| `auth.cpp/.hpp` | Xbox Live device-code sign-in, token cache and refresh |
| `catalog.cpp/.hpp` | Game Pass catalog and per-title metadata |
| `http.cpp/.hpp` | blocking libcurl wrapper, one instance per thread |
| `session.cpp/.hpp` | GSSV session lifecycle, SDP and ICE exchange, Teredo decode |
| `xcloud_protocol.cpp/.hpp` | control/input channel message and gamepad packet formats |

## Notes for anyone touching these

- **`Http` has no `curl_global_init()`.** `curl_easy_init()` does a lazy
  implicit global init, which libcurl documents as *not* thread-safe. The app
  gets away with it because the first `Http` is constructed on the main thread
  before any worker exists. If a worker ever constructs the first one, that is a
  latent startup race — call `curl_global_init(CURL_GLOBAL_DEFAULT)` once in
  `main()` rather than relying on the ordering.
- **`Http`'s 15 s `CURLOPT_TIMEOUT` is whole-transfer, not idle.** The library
  screen's name resolution fetches roughly 30 responses of about 1 MB; on a weak
  link that is a real ceiling, and it fails the whole request rather than
  slowing down. `CURLOPT_LOW_SPEED_LIMIT`/`_TIME` expresses "stalled" better
  than a wall-clock cap does.
- **`decode_teredo()` requires the fully expanded 8-group IPv6 form.** A
  compressed `2001:0:...::...` returns false and the candidate is dropped. It
  has not been seen compressed in practice, but it is not guarded either.
