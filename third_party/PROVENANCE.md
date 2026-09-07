# third_party/ — provenance

Everything under this directory is a **local copy** that is compiled into the
client. None of it is a live clone, a submodule, or a patch applied to
something fetched at build time. What you read here is what gets built.

That is the whole point of the directory: before 2026-09-05 the build compiled
green-nx's `src/core/` straight out of `reference/`, and rebuilt the WebRTC
stack by shallow-fetching a pinned libpeer commit from GitHub and applying a
1191-line patch to it. Both worked until they didn't: a `git pull` in
`reference/`, an upstream force-push, or GitHub garbage-collecting an
unreferenced commit would each have changed or broken the build with no
warning and nothing local to fall back on.

`reference/` is now what its name says — read-only upstream clones kept for
comparison, referenced by no build script.

## Contents

| Path | Upstream | Version / commit | Licence | Modified? |
|---|---|---|---|---|
| `nlohmann/json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | no |
| `libpeer/` | [sepfy/libpeer](https://github.com/sepfy/libpeer) | `9319aa434cb9e893faed0293ba9d2a21eca59c8b` | MIT | **yes** — see below |
| `libpeer/third_party/mbedtls` | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | 3.4.0, `1873d3bfc2da771672bd8e7e8f41f57e0af77f33` | Apache-2.0 | one config line, plus pruning |
| `libpeer/third_party/libsrtp` | [cisco/libsrtp](https://github.com/cisco/libsrtp) | 2.4.2, `90d05bf8980d16e4ac3f16c19b77e296c4bc207b` | BSD-3-Clause | pruning only |
| `libpeer/third_party/usrsctp` | [sctplab/usrsctp](https://github.com/sctplab/usrsctp) | `01cc4e042e2235b29d9d489d89728a6f9ac063ed` | BSD-3-Clause | pruning only |

"Pruning" means whole directories were deleted; no file that remains was
edited, except mbedtls's `mbedtls_config.h`. Both are described below.

The green-nx core (GPL-3.0) is **not** here. It lives in `src/gnx/` because it
is first-party code now — we build it, we modify it, and its licence is the
one the finished client inherits. See `src/gnx/PROVENANCE.md`.

## What was changed in libpeer

The full diff against pristine upstream is preserved at
`deps/patches/libpeer-rg353.patch`. **Nothing applies it any more**; it is kept
as the record of what these files are, so a future re-vendor can tell an
intentional change from an upstream one. In summary:

- **SDP offer template** (`sdp.c`) rewritten to mirror a known-good native
  xCloud client. xCloud pattern-matches the video m-line against per-device
  codec templates; a half-match gets the video track rejected outright, and a
  miss lands in a fallback whose encoder is pinned near 150 kbps.
- **Raw RTP passthrough** (`rtp.c`). libpeer's own in-arrival-order NALU
  assembly corrupts frames on any reorder or loss. Both decoders now hand the
  whole RTP packet — header included, so the caller can read the sequence
  number — to our jitter buffers, which do reassembly, reordering and keyframe
  gating.
- **DTLS role, handshake bio and retransmission** (`dtls_srtp.c`,
  `peer_connection.c`). The stashed-datagram bug that wedged the worker
  forever, and the `dtls_srtp->state` gate that killed every handshake after
  the 7 s retransmit ladder, are both fixed here.
- **ICE**: candidate-pair ordering, ICE consent freshness (RFC 7675, which
  upstream does not implement at all), and a synthetic-candidate path for
  xCloud's Teredo-encoded endpoint (`agent.c`).
- **RTCP feedback**: PLI/FIR, NACK, REMB and Receiver Reports with LSR/DLSR.
  Without valid receiver feedback a libwebrtc sender pins its encoder at the
  starvation floor.
- **Per-stream SCTP reliability** (`sctp.c`) so the input channel is reliable
  and ordered while others are not.
- **`CONFIG_MAX_NALU_SIZE` raised to 2 MB** (`config.h`). Upstream's 10 KB
  truncated every 720p keyframe mid-reassembly.
- **`SO_RCVBUF` request of 4 MB** (`socket.c`). Note this is a *request*: Linux
  silently clamps it to `net.core.rmem_max`, which on the RG353's stock
  firmware is 208 KB.
- **`LOG_REDIRECT`** so libpeer's logs land in our `peer_log()`
  (`src/net/peer_compat.c`) instead of stdout.

## What was pruned

The three dependency trees were copied whole and then had directories nothing
in our build compiles removed, taking `third_party/` from about 65 MB to 14 MB:

- mbedtls: `tests/` (28 MB), `programs/`, `visualc/`, `docs/`, `doxygen/`,
  `ChangeLog`, `3rdparty/everest/`
- libsrtp: `fuzzer/` (4.3 MB), `test/`, `doc/`
- usrsctp: `fuzzer/` (2.7 MB), `programs/`, `Manual.tex`, `Manual.md`

`3rdparty/everest` went for a second reason as well as size. It holds the
optional Everest/HACL* Curve25519, gated behind
`MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED`, which is commented out in our config —
`3rdparty/CMakeLists.txt` only does `add_subdirectory(everest)` when that
setting is on, so it was never compiled. It also contained the two longest
paths in the whole repository, including a 133-character
`FStar_UInt64_FStar_UInt32_FStar_UInt16_FStar_UInt8.h`, which **broke
`git clone` on Windows** with "Filename too long" as soon as the destination
directory was moderately deep. Without it the longest tracked path is 110
characters, leaving 150 for the clone root.

The references left behind are all inert under CMake: `library/CMakeLists.txt`
guards on `if(TARGET everest)`, which is false when the subdirectory was never
added, and the stale `-I .../3rdparty/everest/include` on line 299 of the root
`CMakeLists.txt` is a non-existent include path, which compilers ignore. Both
build targets were rebuilt from scratch after the prune to confirm it.

Two caveats. `3rdparty/Makefile.inc` unconditionally includes
`everest/Makefile.inc`, so mbedtls's **Makefile** build would now fail — we use
CMake exclusively, but do not reach for `make` in that tree. And turning
Everest on would need the directory restored first.

The build already passed `-DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF`,
`-DTEST_APPS=OFF` and `-Dsctp_build_programs=0`, so none of it was being
compiled. **`GEN_FILES=OFF` matters**: mbedtls then uses the pre-generated
`error.c`, `version_features.c`, `ssl_debug_helpers_generated.c` and
`psa_crypto_driver_wrappers.c` checked in under `library/`, which is why
`scripts/` was kept and those four files must not be deleted.

libpeer also pulls `cJSON`, `coreHTTP` and `coreMQTT` as submodules. They serve
only its MQTT/HTTP signalling, which we build with `-DDISABLE_PEER_SIGNALING=ON`,
so 5.9 MB of them was fetched on every fresh setup and never compiled. They are
not vendored and are no longer fetched.

## Four mbedtls files are tracked against upstream's own `.gitignore`

`library/error.c`, `library/version_features.c`,
`library/ssl_debug_helpers_generated.c` and
`library/psa_crypto_driver_wrappers.c` are listed in mbedtls's own
`library/.gitignore`, because upstream generates them with Python at build
time. We build with `GEN_FILES=OFF` — deliberately, so the build container
needs no Python — which means we need them **checked in**.

Git honours nested `.gitignore` files and a deeper one beats a shallower one,
so no rule in the root `.gitignore` can override it. They were added once with
`git add -f`; ignore rules do not apply to already-tracked files, so ordinary
`git add -A` picks up changes to them from now on.

**If you ever re-vendor mbedtls, check this again**, because deleting and
re-copying the tree drops them back to untracked and they will silently vanish
from the next clone — which then fails to configure. The check is:

```bash
git ls-files --others --ignored --exclude-standard third_party/ | grep -v build-
```

Anything it lists is a file the repo has but a fresh clone would not.

## Build detritus lives in here too

`deps/build-deps.sh` configures CMake in-tree, so after a build you will find
`build-rg353/` and `build-host/` inside each of the four project directories —
about 15 MB on top of the 14 MB of source. They are outputs, not content:
delete them freely, and exclude them if this ever gets a `.gitignore`:

```
third_party/**/build-rg353/
third_party/**/build-host/
```

The previous arrangement did the same thing inside `deps/src/libpeer`, so this
is not new; it is just now somewhere you might mistake it for source.

## The one mbedtls change

`include/mbedtls/mbedtls_config.h` has `MBEDTLS_SSL_DTLS_SRTP` enabled;
upstream ships it commented out and libpeer's DTLS layer does not build without
it. This used to be a `sed` in `deps/build-deps.sh`. It is now simply the state
of the vendored file, and the build **verifies** it rather than editing it — if
a future re-vendor loses the line, the build fails on that check instead of on
two hundred lines of missing symbols.

## Re-vendoring

Do not "update" any of this by re-cloning over the top; the libpeer changes
above are not upstream and would be lost. To move to a newer libpeer:

1. Clone pristine upstream at the new ref somewhere outside this tree.
2. Rebase `deps/patches/libpeer-rg353.patch` onto it hunk by hunk. Upstream's
   2026-08 master breaks every hunk and moves to the mbedtls 4.x layout, which
   is why the old ref was pinned in the first place.
3. Copy the result here, re-prune, rebuild both targets, and **test a real
   stream on the handheld** — the SDP template and the DTLS timing are not
   things the offline replay harness can check.
