# Security

## Reporting a vulnerability

Report privately through GitHub's **Report a vulnerability** button on the
Security tab, which opens a private advisory. Please do not open a public issue
for anything that exposes an account.

Include what you did, what happened, and how confident you are. A proof of
concept helps but is not required to file. Expect an acknowledgement within a
week; this is a spare-time project, so a fix may take longer than that, and the
answer to a real issue with no fix will be a documented workaround rather than
silence.

## What this software touches

Understanding the blast radius is most of the analysis:

- **A Microsoft account.** The client runs the standard OAuth device-code flow
  and stores the resulting **refresh token** on the handheld. That token can
  mint access tokens for Xbox Live until it is revoked. It is the most
  sensitive thing here by a wide margin.
- **A network stream.** WebRTC over DTLS-SRTP, with the media path built on
  vendored mbedTLS, libsrtp and usrsctp.
- **Root on the device.** The stock firmware has one account and the client
  runs as it. It opens `/dev/dri/card0`, raw evdev nodes and ALSA directly, and
  optionally adjusts Wi-Fi power save and the CPU governor for the duration of
  a stream.

## How credentials are handled

- The refresh token is written to `tokens.json` beside the binary, mode
  **0600**, via a write-to-temporary-then-rename so an interrupted or failing
  write cannot destroy a working token.
- Tokens are **never logged**. The client's log goes to
  `/userdata/system/logs/xcloud.log` in the clear, so if you find a code path
  that puts a bearer token, a refresh token or a device code into it, that is a
  bug worth reporting.
- Nothing is transmitted anywhere except Microsoft's own endpoints and the
  media peer they nominate. There is no telemetry, no analytics and no update
  check.
- **To sign out, delete `tokens.json`.** To revoke properly — which is what you
  want if a device is lost or sold — remove the app from
  <https://account.live.com/consent/Manage>, which invalidates the refresh
  token server-side.

### If you are sharing logs or recordings

`scripts/pull.sh` and the `-record` flag produce files that are useful for
debugging and are **not** scrubbed. A recording contains the media stream; a
log contains your account's game library, console identifiers and session IDs.
Look before you attach one to an issue.

## Known exposure

- **`device.env` holds a device password in plaintext** if you choose password
  authentication during setup. It is gitignored, but it is still a secret in a
  file. Key authentication (`DEVICE_TRANSPORT=ssh` with `DEVICE_KEY`) avoids
  this and is supported everywhere.
- **The stock firmware's SSH is root with a well-known default password.** That
  is the device's posture, not this project's, but it is worth knowing: anyone
  on your network can reach the handheld, and therefore the token on it. Change
  the password, or keep the device off untrusted networks.
- **`scripts/push.sh` serves a file over plain HTTP** on the local network for
  the seconds a transfer takes, bound to all interfaces. It carries a build
  artefact, not a credential, but it is unauthenticated while it runs.
- **Vendored dependencies are pinned and will not update themselves.** mbedTLS
  3.4.0, libsrtp 2.4.2 and usrsctp are frozen at the commits in
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). A CVE in any of them
  applies here until the vendored copy is bumped by hand — a real cost of
  vendoring, accepted deliberately so the build cannot change underneath you.
  Reports that one of these is out of date and exploitable in our
  configuration are welcome and in scope.

## Out of scope

- The Anbernic stock firmware's own security posture (root SSH, default
  password, read-only root). Report those upstream.
- Anything requiring physical access to an unlocked device.
- The fact that this client exists at all, or that it may conflict with the
  Xbox terms of service. That is a licensing question, addressed in the
  [README](README.md#legal-and-licensing), not a vulnerability.
