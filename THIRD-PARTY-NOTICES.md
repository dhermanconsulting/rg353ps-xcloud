# Third-party notices

This client is distributed under **GPL-3.0** (see [LICENSE](LICENSE)). That is
not a choice — `src/gnx/` derives from GPL-3.0 code and is compiled in, so the
combined work inherits it.

Everything the client links is listed below. All of it is **vendored in this
repository**: the build fetches no source, clones nothing, and applies no patch
at build time, so what you read here is what gets compiled. Exact upstream
commits and the full account of what was modified are in
[third_party/PROVENANCE.md](third_party/PROVENANCE.md) and
[src/gnx/PROVENANCE.md](src/gnx/PROVENANCE.md).

## Compiled in

| Component | Upstream | Version | Licence | Where |
|---|---|---|---|---|
| green-nx core | [rmrf404/green-nx](https://github.com/rmrf404/green-nx) | `e498290` (2026-08-24) | **GPL-3.0** | `src/gnx/` |
| libpeer | [sepfy/libpeer](https://github.com/sepfy/libpeer) | `9319aa4` | MIT | `third_party/libpeer/` |
| Mbed-TLS | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | 3.4.0 (`1873d3b`) | Apache-2.0 | `third_party/libpeer/third_party/mbedtls/` |
| libsrtp | [cisco/libsrtp](https://github.com/cisco/libsrtp) | 2.4.2 (`90d05bf`) | BSD-3-Clause | `third_party/libpeer/third_party/libsrtp/` |
| usrsctp | [sctplab/usrsctp](https://github.com/sctplab/usrsctp) | `01cc4e0` | BSD-3-Clause | `third_party/libpeer/third_party/usrsctp/` |
| nlohmann/json | [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | `third_party/nlohmann/json.hpp` |

Licence texts ship beside the code they cover:

- GPL-3.0 — [`LICENSE`](LICENSE), and [`src/gnx/LICENSE.GPL-3.0`](src/gnx/LICENSE.GPL-3.0)
- MIT (libpeer) — [`third_party/libpeer/LICENSE`](third_party/libpeer/LICENSE)
- Apache-2.0 (Mbed-TLS) — `third_party/libpeer/third_party/mbedtls/LICENSE`
- BSD-3-Clause (libsrtp) — `third_party/libpeer/third_party/libsrtp/LICENSE`
- BSD-3-Clause (usrsctp) — `third_party/libpeer/third_party/usrsctp/LICENSE.md`

## Linked from the device

These are **not** distributed with the client. They are shared libraries the
stock Anbernic firmware already ships, resolved at load time:

| Library | Licence | Note |
|---|---|---|
| glibc | LGPL-2.1+ | Dynamically linked; ABI ceiling is glibc 2.32 |
| libdrm | MIT | Display output |
| FreeType | FTL or GPL-2.0 | Text; the client also reads the device's own DejaVu fonts rather than bundling any |
| libcurl | curl (MIT-like) | HTTP |
| FFmpeg (libavcodec, libavformat, libavutil) | LGPL-2.1+ | H.264 decode |
| ALSA (libasound) | LGPL-2.1+ | Audio |

`libstdc++` and `libgcc` are linked **statically** (`-static-libstdc++
-static-libgcc`), under the GCC Runtime Library Exception, because the device's
libstdc++ is older than the C++17 the client is built with.

## Modifications

`third_party/` is not pristine. libpeer carries substantial behavioural changes
— the SDP offer template, raw RTP passthrough, DTLS role and retransmission,
ICE consent freshness, RTCP feedback, per-stream SCTP reliability — preserved
as a diff against upstream at
[`deps/patches/libpeer-rg353.patch`](deps/patches/libpeer-rg353.patch) and
described in [third_party/PROVENANCE.md](third_party/PROVENANCE.md).

Mbed-TLS has one changed configuration line (`MBEDTLS_SSL_DTLS_SRTP` enabled).
libsrtp and usrsctp are unmodified apart from deleting directories the build
does not compile.

All of these licences permit modification and redistribution under the terms
above; the modified files retain their original licence and headers.

## Trademarks

"Xbox", "Xbox Game Pass" and "Xbox Cloud Gaming" are trademarks of Microsoft
Corporation. "Anbernic" is a trademark of Shenzhen Anbernic Electronics.
"PortMaster" and "Batocera" belong to their respective projects. They are used
here only to describe what this software interoperates with. This project is
not affiliated with, endorsed by, or sponsored by any of them, and no
trademark licence is claimed or granted.
