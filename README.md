# xcloud-rg353ps

A native Xbox Cloud Gaming client for Anbernic RG353-series handhelds, running
on the stock firmware. Sign in on the device, browse your Game Pass library,
and stream — or connect to your own Xbox over remote play.

No browser, no Android, no PortMaster runtime — one self-contained binary
that talks to Microsoft's streaming service directly, decodes H.264 in
software, and puts frames on a DRM overlay plane at 60 fps. The download is
about 8 MB.

> **Not affiliated with Microsoft or Anbernic.** "Xbox", "Xbox Game Pass" and
> "Xbox Cloud Gaming" are trademarks of Microsoft Corporation; "Anbernic" is a
> trademark of Shenzhen Anbernic Electronics. This is an independent,
> unofficial client that uses your own account and your own subscription. It
> ships no Microsoft code and circumvents no protection. See
> [Legal and licensing](#legal-and-licensing).

## What works

| | |
|---|---|
| **Sign in** | Microsoft device-code flow, on the handheld. Token cached locally. |
| **Library** | Your Game Pass catalog with box art, search, sort, recently played. |
| **Streaming** | 60 fps at 720p with sound, over WebRTC. |
| **Remote play** | Your own Xbox instead of the cloud — lower latency, and not metered against Game Pass hours. Cold consoles are woken automatically. |
| **Input** | Raw evdev gamepad, configurable face-button layout. |
| **Settings** | On-device options menu; no SSH needed to change anything. |

Measured on an RG353PS: a sustained 60 fps with zero dropped frames, zero NACKs
and zero PLIs over 78 seconds; a new frame on every panel refresh with one late
refresh in 102 seconds; audio 30–50 ms behind the network with no loss. Remote
play runs at 41–52 ms end to end against a LAN console, against 55–75 ms for
the cloud.

**The Wi-Fi link is the remaining bottleneck**, not the SoC — decode uses about
a third of a 16.6 ms frame budget. See [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

### Known limits

- **Text is coarse in some games.** The panel is 640x480 and the service
  encodes at 1280x720, so the picture is halved. Some titles honour a
  client-requested 640x360 and look native; most ignore it.
  [docs/RESOLUTION.md](docs/RESOLUTION.md) has the full matrix.
- **Hardware video decode does not work on the stock firmware**, and this is
  not a bug in this client — the firmware's Rockchip MPP library has no HAL for
  the decoder block this SoC actually has. Software decode is fast enough.
  [docs/HARDWARE-DECODE.md](docs/HARDWARE-DECODE.md) has the evidence.
- Open items are tracked in [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md).

## Requirements

**Handheld** — an Anbernic RG353-series device on the stock firmware
(`ANBERNIC LINUX ES v31-dev`, a rebranded Batocera 31). SSH enabled, which it
is out of the box. Nothing is installed into the read-only root; everything
lands under `/userdata`.

**Account** — an Xbox account with Game Pass Ultimate for cloud streaming, or
an Xbox console on the same network for remote play.

**To build** — Docker, and any host that can run it. Nothing else: the whole
dependency stack is vendored in this repository, so the build fetches nothing
from the network.

## Install

**No terminal needed.** Download the latest zip from the
[Releases page](../../releases), unzip it, and copy the `ports` folder onto
your ROMs SD card so it merges with the `ports` folder already there. Put the
card back — it appears under **Ports**.

That is the whole install. Step-by-step, with pictures of the folder layout
and what to do when something goes wrong:
**[docs/INSTALL.md](docs/INSTALL.md)**.

Sign-in happens on the handheld: it shows a code, you enter it at
`microsoft.com/link` on any browser, once.

### From source

```bash
git clone <repository-url> xcloud-rg353ps
cd xcloud-rg353ps
sh scripts/bootstrap.sh        # toolchain images, dependencies, the client
sh scripts/setup-device.sh     # asks for your handheld's address, verifies it
sh scripts/install.sh          # installs onto the handheld over the network
```

Docker is the only prerequisite; `bootstrap.sh` builds the rest and skips any
step already done. `setup-device.sh` writes `device.env`, which is gitignored —
the repository carries no address, password or host key of its own.

`sh scripts/package.sh` builds the release archive described above.

Either way it appears as **Xbox Cloud Gaming** under **Ports**. The port
*merges* its entry into the device's shared `gamelist.xml` rather than
overwriting it, so PortMaster's entries and everything else survive.

Logs land at `/userdata/system/logs/xcloud.log`, and the first line names the
build. Sign-in and settings live in `/userdata/ports/xcloud/`, on the internal
storage rather than the card, so replacing the port does not sign you out.

## Using it

On first run the client shows a code and a `microsoft.com/link` URL. Enter it
in a browser anywhere; the handheld picks up the token and caches it.

| Control | Does |
|---|---|
| **D-pad / sticks** | Navigate |
| **A** | Select |
| **B** | Back |
| **START** (in library) | Settings |
| **SELECT + X** (in a stream) | Options overlay |
| **SELECT + START** | Quit |

Settings are split into **Standard** and **Advanced**, edited as a staged set
and applied on confirm. Rows marked `*` take effect on the next stream rather
than immediately. Everything persists to `options.json` under
`/userdata/ports/xcloud/`.

Every setting also has a command-line flag, which overrides the saved value for
one run — see [docs/BUILDING.md](docs/BUILDING.md#running-on-the-device) for the
full list.

## Developing

You do not need the handheld for most work. The same client builds for x86-64
against a simulated panel, signs in on its own, and runs **real streaming
sessions** from a container:

```bash
sh scripts/build-host.sh
XCLOUD_SIM_VIEW=8080 sh scripts/sim.sh -replay recordings/synth-20s.xcau
```

Then open <http://localhost:8080/> — every presented frame arrives in the
browser as a lossless PNG and the keyboard drives the gamepad. You can open the
options overlay and watch a downscale filter change the same frame, at a desk,
with no hardware.

```bash
sh scripts/test.sh             # the full test bench: PASS/FAIL per check
```

See [docs/TESTING.md](docs/TESTING.md) for the simulator, the record/replay
harness and the network impairment model, and
[CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## Documentation

| | |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | Installing and using it, for people who are not building it |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the client is put together, module by module |
| [docs/BUILDING.md](docs/BUILDING.md) | Toolchain, build images, every command-line flag |
| [docs/TESTING.md](docs/TESTING.md) | Simulator, test bench, record/replay, impairment |
| [docs/DEVICE.md](docs/DEVICE.md) | The hardware, and the constraints it imposes |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | The wire protocol: REST chain, SDP, data channels |
| [docs/VIDEO-PACING.md](docs/VIDEO-PACING.md) | Decode threading, vsync pacing, the audio path |
| [docs/RESOLUTION.md](docs/RESOLUTION.md) | Why the picture is halved, and what can be done |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Where the time actually goes; the cost of a lost frame |
| [docs/REMOTE-PLAY.md](docs/REMOTE-PLAY.md) | Streaming your own console |
| [docs/HARDWARE-DECODE.md](docs/HARDWARE-DECODE.md) | Why the VPU is unusable here, precisely |
| [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) | What is vendored, why, and what reading it turned up |
| [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) | Open items, each with a root cause |

## Legal and licensing

This client is **GPL-3.0**. The account, catalog and session layer in
`src/gnx/` derives from [green-nx](https://github.com/rmrf404/green-nx), which
is GPL-3.0, and is compiled in — so the whole is. Full text in
[LICENSE](LICENSE); per-file provenance in
[src/gnx/PROVENANCE.md](src/gnx/PROVENANCE.md).

Vendored dependencies are MIT, Apache-2.0 and BSD-3-Clause, listed with their
exact upstream commits in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and
[third_party/PROVENANCE.md](third_party/PROVENANCE.md).

**On the service itself.** This is an independent client for a service you pay
for, in the same category as the other open-source xCloud clients it learned
from. It authenticates with your own credentials through Microsoft's public
device-code flow, streams only to the person signed in, contains no Microsoft
code, and bypasses no technical protection measure. It is not endorsed by
Microsoft, and using it may not be consistent with the Xbox terms of service —
that is your call to make, and your account at risk. There is no warranty; see
sections 15–17 of the licence.

## Credits

Standing on the shoulders of [green-nx](https://github.com/rmrf404/green-nx)
for the GSSV protocol layer, [libpeer](https://github.com/sepfy/libpeer) for a
WebRTC stack small enough to fit, and the Batocera and PortMaster projects for
making the device hospitable in the first place.
