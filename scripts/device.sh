#!/bin/sh
# Shared connection settings for every script that talks to the handheld.
# Source it, do not run it:
#
#   . "$(dirname "$0")/device.sh"
#   device_require
#   device_run "uname -a"
#
# There are deliberately NO built-in defaults for the address, password or
# host key. Those describe one person's living room, they go stale the moment
# DHCP moves the device, and a default that is wrong for everybody but its
# author is worse than no default: it turns "you have not configured this yet"
# into "connection refused" against a stranger's machine.
#
# Configuration is read from, in order of precedence:
#
#   1. the environment
#   2. device.env in the repository root, which is gitignored
#
# Copy device.env.example to device.env and fill it in; scripts/setup-device.sh
# will do that for you and verify the result.
#
# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------
#
#   DEVICE_HOST      required. Hostname or address of the handheld.
#   DEVICE_USER      login user. Default root, which is all the stock
#                    firmware offers.
#   DEVICE_TRANSPORT auto | ssh | plink. Default auto: plink when a password
#                    is configured and plink is installed, otherwise ssh.
#   DEVICE_PW        password, used only by plink. OpenSSH cannot accept a
#                    password non-interactively, so ssh needs a key instead.
#   DEVICE_KEY       path to a private key, used by ssh.
#   DEVICE_HOSTKEY   the device's SSH host key fingerprint. Pinned with
#                    plink's -hostkey so an address change cannot silently
#                    move the session to another machine.
#   DEVICE_PLINK     path to plink.exe.
#   DEVICE_SSH_OPTS  extra options passed to ssh.
#
# The device is on DHCP by default and its address will move. A static
# reservation on the router is the cure; failing that, expect to edit
# device.env occasionally.

# ---------------------------------------------------------------------------
# Load device.env, if there is one.
# ---------------------------------------------------------------------------
# Read with `.` so it is plain shell: KEY=value lines, comments allowed.
#
# Found by looking in the obvious places rather than deriving a path from
# $0 -- inside a sourced file $0 belongs to the CALLER, not to this file, so
# `dirname $0` is whatever invoked us. That is the repository root for every
# script here (they all cd there first) but it is the interactive shell when
# someone sources this by hand, and then the config silently is not found.
if [ -z "$DEVICE_ENV" ]; then
	for _cand in ./device.env \
	             "$(dirname -- "$0")/../device.env" \
	             "$(dirname -- "$0")/device.env"; do
		if [ -f "$_cand" ]; then DEVICE_ENV=$_cand; break; fi
	done
fi
if [ -n "$DEVICE_ENV" ] && [ -f "$DEVICE_ENV" ]; then
	# Environment wins, so remember what was already set and put it back.
	_env_host=$DEVICE_HOST; _env_user=$DEVICE_USER; _env_pw=$DEVICE_PW
	_env_key=$DEVICE_KEY;   _env_hostkey=$DEVICE_HOSTKEY
	_env_plink=$DEVICE_PLINK; _env_transport=$DEVICE_TRANSPORT
	. "$DEVICE_ENV"
	[ -n "$_env_host" ]      && DEVICE_HOST=$_env_host
	[ -n "$_env_user" ]      && DEVICE_USER=$_env_user
	[ -n "$_env_pw" ]        && DEVICE_PW=$_env_pw
	[ -n "$_env_key" ]       && DEVICE_KEY=$_env_key
	[ -n "$_env_hostkey" ]   && DEVICE_HOSTKEY=$_env_hostkey
	[ -n "$_env_plink" ]     && DEVICE_PLINK=$_env_plink
	[ -n "$_env_transport" ] && DEVICE_TRANSPORT=$_env_transport
fi

DEVICE_USER=${DEVICE_USER:-root}
DEVICE_TRANSPORT=${DEVICE_TRANSPORT:-auto}
DEVICE_PLINK=${DEVICE_PLINK:-plink}
DEVICE_SSH=${DEVICE_SSH:-ssh}

# ---------------------------------------------------------------------------
# device_require -- refuse to run against an unconfigured target
# ---------------------------------------------------------------------------
# Called at the top of anything that touches the device. The message is the
# whole point: an unconfigured checkout should say what to do, not fail later
# with a connection timeout that looks like a broken device.
device_require() {
	if [ -z "$DEVICE_HOST" ]; then
		cat >&2 <<-'EOF'
		error: no handheld configured.

		  cp device.env.example device.env    and fill in DEVICE_HOST,
		  or run: sh scripts/setup-device.sh

		Or set it for one command:  DEVICE_HOST=192.0.2.10 sh scripts/...
		EOF
		exit 2
	fi

	case "$DEVICE_TRANSPORT" in
	auto)
		if [ -n "$DEVICE_PW" ] && command -v "$DEVICE_PLINK" >/dev/null 2>&1; then
			DEVICE_TRANSPORT=plink
		else
			DEVICE_TRANSPORT=ssh
		fi
		;;
	ssh|plink) ;;
	*)
		echo "error: DEVICE_TRANSPORT must be auto, ssh or plink (got '$DEVICE_TRANSPORT')" >&2
		exit 2
		;;
	esac

	if [ "$DEVICE_TRANSPORT" = plink ]; then
		command -v "$DEVICE_PLINK" >/dev/null 2>&1 || {
			echo "error: plink not found at '$DEVICE_PLINK' -- set DEVICE_PLINK" >&2
			exit 2
		}
		# Unpinned, plink prompts about the host key and -batch then aborts.
		# Say that plainly rather than letting it read as a network fault.
		[ -n "$DEVICE_HOSTKEY" ] || {
			echo "warning: DEVICE_HOSTKEY is unset; plink will refuse an unknown host in batch mode." >&2
			echo "         Connect once interactively to record it, or run scripts/setup-device.sh." >&2
		}
	else
		command -v "$DEVICE_SSH" >/dev/null 2>&1 || {
			echo "error: ssh not found -- install OpenSSH, or set DEVICE_TRANSPORT=plink" >&2
			exit 2
		}
	fi
}

# ---------------------------------------------------------------------------
# device_run <command> -- run one shell command on the handheld
# ---------------------------------------------------------------------------
# stdin is inherited, so this also works as the receiving end of a pipe.
# Both transports run non-interactively and never prompt: a script that stops
# waiting for a human is indistinguishable from a hang.
# The command is copied out of $1 immediately, because both branches then
# rebuild the positional parameters to assemble their own option list.
device_run() {
	_cmd=$1
	if [ "$DEVICE_TRANSPORT" = plink ]; then
		set -- -ssh -batch
		[ -n "$DEVICE_HOSTKEY" ] && set -- "$@" -hostkey "$DEVICE_HOSTKEY"
		[ -n "$DEVICE_PW" ] && set -- "$@" -pw "$DEVICE_PW"
		[ -n "$DEVICE_KEY" ] && set -- "$@" -i "$DEVICE_KEY"
		"$DEVICE_PLINK" "$@" "$DEVICE_USER@$DEVICE_HOST" "$_cmd"
	else
		set -- -o BatchMode=yes -o ConnectTimeout=10
		[ -n "$DEVICE_KEY" ] && set -- "$@" -i "$DEVICE_KEY"
		# shellcheck disable=SC2086  # deliberately word-split: caller-supplied options
		"$DEVICE_SSH" "$@" $DEVICE_SSH_OPTS "$DEVICE_USER@$DEVICE_HOST" "$_cmd"
	fi
}

# ---------------------------------------------------------------------------
# device_target -- "user@host", for messages and URLs
# ---------------------------------------------------------------------------
device_target() { printf '%s@%s' "$DEVICE_USER" "$DEVICE_HOST"; }
