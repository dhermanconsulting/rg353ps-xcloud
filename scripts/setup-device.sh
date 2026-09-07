#!/bin/sh
# Write device.env and prove it works, so the first thing a new checkout does
# is not a connection timeout.
#
#   sh scripts/setup-device.sh
#
# Interactive by design: it asks, writes device.env, then runs one command on
# the handheld and prints what came back. Nothing else in scripts/ prompts --
# a build or a push that stops waiting for a human is indistinguishable from
# a hang -- so this is the one place it happens.
#
# Re-running it is safe: existing answers become the defaults and an existing
# device.env is backed up before it is replaced.
set -e
cd "$(dirname "$0")/.."

ENV_FILE=${DEVICE_ENV:-device.env}

# Load what is already there so the prompts can offer it back.
[ -f "$ENV_FILE" ] && . "./$ENV_FILE"

ask() {  # ask <prompt> <default> -> answer on stdout
	_prompt=$1; _default=$2
	if [ -n "$_default" ]; then
		printf '%s [%s]: ' "$_prompt" "$_default" >&2
	else
		printf '%s: ' "$_prompt" >&2
	fi
	read -r _answer || _answer=
	[ -n "$_answer" ] || _answer=$_default
	printf '%s' "$_answer"
}

echo "Setting up the handheld connection. Ctrl-C to stop." >&2
echo >&2
echo "The address is in EmulationStation under Main Menu -> Network Settings," >&2
echo "or in your router's client list. The firmware uses DHCP, so it moves." >&2
echo >&2

HOST=$(ask "Device address" "$DEVICE_HOST")
[ -n "$HOST" ] || { echo "setup: no address given, nothing written" >&2; exit 2; }
USER_=$(ask "Login user" "${DEVICE_USER:-root}")

echo >&2
echo "Authentication:" >&2
echo "  1) password  (uses PuTTY's plink; the usual choice on Windows)" >&2
echo "  2) ssh key   (uses OpenSSH; no secret stored in device.env)" >&2
CHOICE=$(ask "Choose 1 or 2" "1")

PW=; KEY=; TRANSPORT=auto
case "$CHOICE" in
2)
	TRANSPORT=ssh
	KEY=$(ask "Private key path" "${DEVICE_KEY:-$HOME/.ssh/id_ed25519}")
	echo >&2
	echo "Install the public half on the device if you have not already:" >&2
	echo "  ssh-copy-id -i $KEY $USER_@$HOST" >&2
	;;
*)
	TRANSPORT=plink
	# Batocera's documented default. Offered as a default here because it is
	# public knowledge and the alternative is a blank prompt on a device most
	# people never change it on -- but it is read back from device.env on a
	# re-run, so anyone who has changed it keeps their own.
	PW=$(ask "Password" "${DEVICE_PW:-linux}")
	;;
esac

PLINK_PATH=$DEVICE_PLINK
if [ "$TRANSPORT" = plink ] && ! command -v "${PLINK_PATH:-plink}" >/dev/null 2>&1; then
	# The Windows installer's default location, which is not on PATH.
	for guess in "/c/Program Files/PuTTY/plink.exe" \
	             "/c/Program Files (x86)/PuTTY/plink.exe"; do
		[ -x "$guess" ] && { PLINK_PATH=$guess; break; }
	done
	[ -n "$PLINK_PATH" ] || PLINK_PATH=$(ask "Path to plink" "plink")
fi

if [ -f "$ENV_FILE" ]; then
	cp "$ENV_FILE" "$ENV_FILE.bak"
	echo >&2
	echo "setup: previous $ENV_FILE saved as $ENV_FILE.bak" >&2
fi

# Every value is written quoted. device.env is read with `.`, so it is shell:
# an unquoted value containing a space -- "/c/Program Files/PuTTY/plink.exe"
# being the one everybody hits -- parses as a command prefix assignment and
# the setting silently does not apply at all. Single quotes, with any embedded
# single quote escaped the usual '\'' way.
q() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }

{
	echo "# Written by scripts/setup-device.sh. Gitignored: it describes your"
	echo "# network. See device.env.example for every setting."
	echo "DEVICE_HOST=$(q "$HOST")"
	echo "DEVICE_USER=$(q "$USER_")"
	echo "DEVICE_TRANSPORT=$(q "$TRANSPORT")"
	[ -n "$PW" ]         && echo "DEVICE_PW=$(q "$PW")"
	[ -n "$KEY" ]        && echo "DEVICE_KEY=$(q "$KEY")"
	[ -n "$PLINK_PATH" ] && echo "DEVICE_PLINK=$(q "$PLINK_PATH")"
	[ -n "$DEVICE_HOSTKEY" ] && echo "DEVICE_HOSTKEY=$(q "$DEVICE_HOSTKEY")"
} > "$ENV_FILE"
echo "setup: wrote $ENV_FILE" >&2

# ---- prove it -------------------------------------------------------------
# plink in batch mode refuses an unrecorded host key, which is the correct
# behaviour and also the most likely reason a first run fails. Catch it here,
# where there is a human to read the explanation, rather than inside a push.
echo >&2
echo "setup: connecting to $USER_@$HOST ..." >&2

. scripts/device.sh
device_require

if OUT=$(device_run "uname -srm; cat /etc/os-release 2>/dev/null | head -2" 2>&1); then
	echo "setup: OK" >&2
	echo "$OUT" | sed 's/^/  /'
	echo >&2
	echo "Next: sh scripts/build-app.sh && sh scripts/install.sh" >&2
else
	echo "setup: could not run a command on the device." >&2
	echo "$OUT" | sed 's/^/  /' >&2
	echo >&2
	case "$OUT" in
	*"host key"*|*"HOST KEY"*|*"fingerprint"*)
		echo "The host key is not recorded yet. Connect once interactively so it can be" >&2
		echo "accepted and stored, then run this again:" >&2
		if [ "$TRANSPORT" = plink ]; then
			echo "  \"$PLINK_PATH\" -ssh $USER_@$HOST" >&2
		else
			echo "  ssh $USER_@$HOST" >&2
		fi
		;;
	*)
		echo "Check that the device is awake and on the network -- it suspends, and a" >&2
		echo "suspended handheld looks exactly like one that is switched off. Its" >&2
		echo "address also changes, because the firmware uses DHCP." >&2
		;;
	esac
	exit 1
fi
