#!/bin/sh
set -eu

DIST="${STREQ_DIST:-https://cloud.openipc.ru/cerberus}"
API="${STREQ_API:-https://cloud-api.openipc.ru/v1}"
DOCS="${STREQ_DOCS:-https://cloud.openipc.ru/docs/openipc-camera/#codes}"
TOKEN="${1:-}"

say() { echo "cerberus: $*"; }
die() { echo "cerberus: E$1" >&2; echo "cerberus: $DOCS" >&2; exit "$1"; }

[ "$(id -u)" = 0 ] || die 10

command -v fw_printenv >/dev/null 2>&1 || die 11
chip=$(fw_printenv -n soc 2>/dev/null | tr 'A-Z' 'a-z' | tr -d ' \t\r') || die 12
[ -n "$chip" ] || die 12
say "chip $chip"

tmp="/tmp/cerberus.$$"
rc="/tmp/S99cerberus.$$"
trap 'rm -f "$tmp" "$tmp.sha" "$rc"' EXIT INT TERM

if ! curl -fsL -o "$tmp" "$DIST/cerberus_$chip"; then
    curl -fsL -m 5 -o /dev/null -X POST -H 'Content-Type: application/json'         -d "{\"chip\":\"$chip\"}" "$API/builds/missing" 2>/dev/null || true
    die 13
fi
curl -fsL -o "$rc" "$DIST/S99cerberus" || die 14

if curl -fsL -o "$tmp.sha" "$DIST/cerberus_$chip.sha256" 2>/dev/null; then
    want=$(cut -d' ' -f1 <"$tmp.sha")
    have=$(sha256sum "$tmp" | cut -d' ' -f1)
    [ "$want" = "$have" ] || die 15
    say "sha256 $have"
fi

if [ -x /etc/init.d/S99cerberus ]; then
    /etc/init.d/S99cerberus stop 2>/dev/null || true
fi

chmod 0755 "$tmp" "$rc" || die 16
mv "$tmp" /usr/sbin/cerberus || die 16
mv "$rc" /etc/init.d/S99cerberus || die 16
mkdir -p /etc/streq || die 16
chmod 0700 /etc/streq || die 16
say "installed /usr/sbin/cerberus"

if [ -z "$TOKEN" ]; then
    say "E20"
    say "$DOCS"
    echo "    fw_setenv streq_api '$API'"
    echo "    fw_setenv streq_enrollment_token 'tok_...'"
    echo "    /etc/init.d/S99cerberus restart"
    exit 0
fi

fw_setenv streq_api "$API" || die 17
fw_setenv streq_enrollment_token "$TOKEN" || die 17

/etc/init.d/S99cerberus restart >/dev/null 2>&1 || die 18
say "started"

claim=""
i=0
while [ "$i" -lt 40 ]; do
    claim=$(/usr/sbin/cerberus status 2>/dev/null | awk '$1 == "claim" && $2 == "code" { print $3 }')
    if [ -n "$claim" ] && [ "$claim" != "-" ]; then
        break
    fi
    claim=""
    i=$((i + 1))
    sleep 1
done

if [ -n "$claim" ]; then
    say "claim $claim"
else
    say "E21"
    say "$DOCS"
    echo "    cerberus status"
fi
