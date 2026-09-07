#!/bin/sh
# Refuse to let private details into the published tree.
#
#   sh scripts/check-clean.sh
#
# This repository was extracted from a personal working tree that had a home
# network address, an SSH host key fingerprint and a public IP scattered
# through its scripts and notes. All of it is gone; this is what keeps it
# gone. It runs in CI and is worth running before a commit.
#
# It checks only files git tracks, so an ignored device.env or a local
# recording is none of its business.
#
# Vendored code is exempt: third_party/ is somebody else's, it legitimately
# contains example addresses in test vectors and comments, and we do not edit
# it. So is this file, which necessarily contains the patterns it looks for.
set -e
cd "$(dirname "$0")/.."

FAIL=0
note() { echo "  $*" >&2; }
fail() { echo "FAIL $1" >&2; shift; [ $# -gt 0 ] && printf '%s\n' "$@" | sed 's/^/       /' >&2; FAIL=1; }
pass() { echo "PASS $1" >&2; }

# Files git would publish: tracked, plus untracked ones that are not ignored,
# so this catches a new file before it has ever been committed. Minus the
# exemptions above.
files() {
	git ls-files --cached --others --exclude-standard |
		grep -v '^third_party/' |
		grep -v '^scripts/check-clean\.sh$'
}

scan() {  # scan <name> <extended-regex> <explanation>
	name=$1; re=$2; why=$3
	hits=$(files | xargs grep -InE "$re" 2>/dev/null | head -20 || true)
	if [ -n "$hits" ]; then
		fail "$name" "$why" "" "$hits"
	else
		pass "$name"
	fi
}

echo "Checking the tracked tree for private details." >&2
echo >&2

# --- addresses ------------------------------------------------------------
# RFC 1918 and RFC 5737 addresses are how you write an example, so the rule is
# not "no addresses" -- it is "no addresses that are somebody's actual host".
# A literal in a script default or a doc is the thing to catch; the RFC 5737
# documentation ranges (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24) and the
# obvious placeholders are allowed.
# Written as three patterns rather than one alternation: the prefixes are
# different lengths, so a shared "three more octets" tail would need five
# octets to match 192.168.x.y and would quietly never fire.
RFC1918='(192\.168\.[0-9]{1,3}\.[0-9]{1,3})|(\b10\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3})|(\b172\.(1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}\.[0-9]{1,3})'
hits=$(files | xargs grep -InoE "$RFC1918" 2>/dev/null |
	grep -vE '\.x$|\.X$|172\.17\.0\.' | head -20 || true)
if [ -n "$hits" ]; then
	fail "no-private-ip" \
	     "A specific RFC 1918 address is somebody's real host. Use an RFC 5737" \
	     "documentation address (192.0.2.x) or a placeholder instead." "" "$hits"
else
	pass "no-private-ip"
fi

# Routable addresses. Matching four bare octets has an unusable false-positive
# rate here -- "gcc 10.2.1", "1.1.1.1" as a ping target, version strings -- so
# this looks only for an address with a port on it, which is unambiguously an
# endpoint rather than a version number. That is the shape the leaked public
# IP had (an ICE candidate, "203.0.113.7:3074").
hits=$(files | xargs grep -InoE '\b(([1-9][0-9]?|1[0-9]{2}|2[0-2][0-9])\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}):[0-9]+' 2>/dev/null |
	grep -vE '(127\.0\.0\.1|0\.0\.0\.0|192\.0\.2|198\.51\.100|203\.0\.113|10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.)' |
	head -20 || true)
if [ -n "$hits" ]; then
	fail "no-public-ip" "A routable address with a port is a real endpoint." "" "$hits"
else
	pass "no-public-ip"
fi

# --- credentials and keys -------------------------------------------------
scan "no-ssh-fingerprint" \
	'\b([0-9a-f]{2}:){9,}[0-9a-f]{2}\b' \
	"That looks like an SSH host key fingerprint. Pin it in device.env, not here."

scan "no-private-key" \
	'BEGIN (RSA|OPENSSH|DSA|EC|PGP) PRIVATE KEY' \
	"A private key must never be committed."

scan "no-bearer-token" \
	'(eyJ[A-Za-z0-9_-]{20,}\.|Bearer [A-Za-z0-9_.-]{20,}|refresh_token"[[:space:]]*:[[:space:]]*"[A-Za-z0-9])' \
	"That looks like a live token."

# A password assigned a literal default in a script is the pattern that put
# the device password in six files. DEVICE_PW=${DEVICE_PW:-...} is the shape.
scan "no-default-password" \
	'(PASS|PASSWORD|_PW)=\$\{[A-Za-z_]+:-[^}]' \
	"A password with a built-in default. Read it from device.env instead."

# --- local paths ----------------------------------------------------------
scan "no-local-paths" \
	'([A-Za-z]:\\[A-Za-z]|/home/[a-z]+/|/Users/[A-Za-z]+/|\.claude/)' \
	"An absolute path from one person's machine."

# --- links ----------------------------------------------------------------
# Every docs/ link in the code and the docs must resolve, or the published
# tree points at something the reader cannot open. This is how the two
# already-dangling references were found.
missing=""
for d in $(files | xargs grep -rhoE 'docs/[A-Za-z0-9-]+\.md' 2>/dev/null | sort -u); do
	[ -f "$d" ] || missing="$missing $d"
done
if [ -n "$missing" ]; then
	fail "docs-links-resolve" "Referenced but not present:$missing"
else
	pass "docs-links-resolve"
fi

echo >&2
if [ "$FAIL" = "0" ]; then
	echo "check-clean: OK" >&2
	exit 0
fi
echo "check-clean: FAILED -- see above." >&2
echo "If a hit is a deliberate example, use an RFC 5737 address or a placeholder." >&2
exit 1
