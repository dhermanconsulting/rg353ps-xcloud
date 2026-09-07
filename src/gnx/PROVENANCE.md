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

Two, both applied when the files were copied on 2026-09-05:

- `#include "../../vendor/json.hpp"` became
  `#include "../../third_party/nlohmann/json.hpp"` in `auth.cpp`,
  `catalog.cpp`, `session.cpp` and `xcloud_protocol.cpp`. The header itself is
  byte-identical to the one green-nx vendored (nlohmann/json 3.11.3, MIT).
- Line endings normalised from CRLF to LF, matching the rest of `src/`. The
  `reference/` clone has CRLF because it was checked out on Windows.

Nothing else — but **compare with `diff --strip-trailing-cr`** or the line
endings alone make every line look changed:

```bash
diff --strip-trailing-cr -r reference/green-nx/src/core src/gnx
```

which should show those four include lines and nothing more. Worth keeping true
while it is cheap: it means an upstream fix can still be read across directly.

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
