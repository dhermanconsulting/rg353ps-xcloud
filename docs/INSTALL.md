# Installing

For people who want to use this, not build it. No terminal, no Docker, no
account on anything. If you want to build from source instead, see
[BUILDING.md](BUILDING.md).

## What you need

- An **Anbernic RG353PS** or another RG353-series handheld, on the **stock
  firmware** (`ANBERNIC LINUX ES v31-dev`, a rebranded Batocera). Nothing is
  installed into the system; everything lands in your ROMs folder.
- **Xbox Game Pass Ultimate**, for cloud streaming — or **your own Xbox** on
  the same network, for remote play, which does not use cloud hours and is
  lower latency.
- **5 GHz Wi-Fi**, ideally. The wireless link, not the handheld, is what
  limits picture quality.

## Install by SD card

This is the whole install. It takes about two minutes.

1. **Download** the latest `xcloud-rg353ps-*.zip` from the
   [Releases page](https://github.com/dhermanconsulting/rg353ps-xcloud/releases)
   and unzip it on your computer.

2. **Turn the handheld off** and take out the SD card that holds your ROMs.
   On a two-slot device that is the second slot — the card with folders like
   `snes`, `psx` and `ports` on it.

3. **Put the card in your computer.** It is exFAT, so Windows, macOS and Linux
   all read and write it without anything extra.

4. **Copy the `ports` folder** from the unzipped archive into the **root of
   the card**, so it merges with the `ports` folder already there. Your
   computer will ask whether to merge — say yes.

   Nothing of yours is replaced. The only new items are `Xbox Cloud.sh` and a
   folder called `xcloud`.

   ```
   SD card root
   ├── ports
   │   ├── PortMaster.sh        <- yours, untouched
   │   ├── gamelist.xml         <- yours, untouched
   │   ├── Xbox Cloud.sh        <- new
   │   └── xcloud/              <- new
   ├── snes
   └── psx
   ```

5. **Eject the card properly**, put it back, and turn the handheld on.

6. It is in the **Ports** section. The first launch registers its proper name,
   so it may show as "Xbox Cloud" until you have run it once, and
   **"Xbox Cloud Gaming"** afterwards.

### Installing over the network instead

If the handheld already shares `/userdata` on your network, or you are happy
with SSH, copy the unzipped folder to `/userdata` and run:

```bash
cd /userdata/xcloud-rg353ps-*
sh install-on-device.sh
```

Same result, plus it restarts EmulationStation so the entry appears without a
reboot.

## First run

The client shows a **code** and the address `microsoft.com/link`.

1. Open that address on your phone or computer.
2. Sign in to your Microsoft account.
3. Enter the code.

The handheld picks it up within a few seconds. **You only do this once** — the
sign-in is remembered at `/userdata/ports/xcloud/tokens.json`.

To sign out, delete that file. To revoke access properly — which is what you
want if you sell or lose the device — remove the app at
<https://account.live.com/consent/Manage>.

## Controls

| | |
|---|---|
| **D-pad / sticks** | Navigate |
| **A** | Select |
| **B** | Back |
| **START** (in the library) | Settings |
| **SELECT + X** (during a stream) | Options |
| **SELECT + START** | Quit |

Everything worth changing is in the on-device settings — stream quality,
face-button layout, picture mode, console language, Wi-Fi tuning. You do not
need a computer again after signing in.

## Updating

Copy the new `ports` folder over the old one, exactly as you did to install.
Your sign-in and settings live on the internal storage, not the card, so they
survive — and so does your play history.

## If something goes wrong

The log is at **`/userdata/system/logs/xcloud.log`** and its **first line names
the exact version**, which is the first thing to include in a bug report.

**Nothing appears in the Ports menu.**
The `xcloud` folder has to sit next to `Xbox Cloud.sh`, both inside the card's
`ports` folder. Copying only the `.sh` file is the usual mistake. Some firmware
also needs a restart before it rescans — reboot the handheld.

**It launches and drops straight back to the menu.**
Read the log. `no client binary found` means the `xcloud` folder did not come
across; copy the whole `ports` folder again.

**It says the port is not executable.**
Rare, and it means the card was mounted in a way that stripped the executable
bit. Reinstalling over the network with `install-on-device.sh` fixes it.

**Stutter, or the picture keeps softening.**
Wi-Fi, nearly always. Get closer to the router and prefer a 5 GHz network. The
handheld's own signal strength is the single biggest factor in how this looks.

**Text is coarse or hard to read.**
Expected, and not something a setting fully fixes. The panel is 640x480 and the
service encodes 1280x720, so the picture is halved on the way in. Try **Stream
quality → 720p high** in settings, which spends more bits on exactly the fine
detail the downscale destroys, and the **Picture mode** options.
[RESOLUTION.md](RESOLUTION.md) explains why in full.

**Sign-in fails or the code expires.**
Codes are short-lived; start the sign-in again and enter it promptly. If the
handheld's clock is badly wrong, TLS to Microsoft fails — set the date in
EmulationStation's settings, or connect it to the internet so it syncs.

## Uninstalling

Delete `Xbox Cloud.sh` and the `xcloud` folder from the card's `ports` folder.
To remove the saved sign-in and settings as well, delete
`/userdata/ports/xcloud`. The entry may linger in the Ports list until
EmulationStation next rebuilds its gamelist.
