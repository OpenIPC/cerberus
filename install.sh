#!/bin/sh
set -eu

DIST="${CERBERUS_DIST:-${STREQ_DIST:-https://github.com/OpenIPC/cerberus/releases/latest/download}}"
API="${CERBERUS_API:-${STREQ_API:-https://cloud-api.openipc.ru/v1}}"
DOCS="${CERBERUS_DOCS:-${STREQ_DOCS:-https://cloud.openipc.ru/docs/openipc-camera/#codes}}"

AGENT=/usr/sbin/cerberus
INIT=/etc/init.d/S99cerberus
CONF=/etc/cerberus
HOOK=/usr/sbin/motion.sh
KEYS="node_id overlay_ip hub_endpoint hub_pubkey allowed_ips network_subnet hub_ip
      claim_code device_key api enrollment_token hostname kind iface subnet ca heartbeat
      ping_target ping_interval ping_timeout ping_fails events snapshot_url"

say() { echo "cerberus: $*"; }
die() { echo "cerberus: E$1" >&2; echo "cerberus: $DOCS" >&2; exit "$1"; }

stop_agent() {
    if [ -x "$INIT" ]; then
        "$INIT" stop >/dev/null 2>&1 || true
    fi
}

uninstall() {
    command -v fw_setenv >/dev/null 2>&1 || die 11

    stop_agent

    iface=$(fw_printenv -n cerberus_iface 2>/dev/null || fw_printenv -n streq_iface 2>/dev/null || true)
    ip link del dev "${iface:-wg0}" >/dev/null 2>&1 || true

    for name in $KEYS; do
        for prefix in cerberus streq; do
            if fw_printenv -n "${prefix}_$name" >/dev/null 2>&1; then
                fw_setenv "${prefix}_$name" >/dev/null 2>&1 || die 30
            fi
        done
    done

    if grep -q CERBERUS_HOOK=1 "$HOOK" 2>/dev/null; then
        if [ -e "$HOOK.orig" ]; then
            mv -f "$HOOK.orig" "$HOOK" || die 30
        else
            rm -f "$HOOK" || die 30
        fi
    fi

    rm -f "$INIT" || die 30
    rm -f "$AGENT" || die 30
    rm -rf "$CONF" /etc/streq || die 30
    say "removed"
}

TOKEN=""
case "${1:-}" in
    --uninstall) MODE=uninstall ;;
    -*) die 19 ;;
    *) MODE=install; TOKEN="${1:-}" ;;
esac

[ "$(id -u)" = 0 ] || die 10

if [ "$MODE" = uninstall ]; then
    uninstall
    exit 0
fi

command -v fw_printenv >/dev/null 2>&1 || die 11
chip=$(fw_printenv -n soc 2>/dev/null | tr 'A-Z' 'a-z' | tr -d ' \t\r') || chip=""
machine=$(uname -m)

case "$machine" in
    aarch64|arm64) arch=aarch64 ;;
    arm*) arch=arm ;;
    mips*)
        if [ "$(dd if=/bin/sh bs=1 skip=5 count=1 2>/dev/null | od -An -tx1 | tr -d ' ')" = 01 ]; then
            arch=mipsel
        else
            arch=""
        fi
        ;;
    x86_64|amd64) arch=x86_64 ;;
    *) arch="" ;;
esac

say "chip ${chip:-?}, cpu $machine"

if [ -z "$arch" ]; then
    curl -fsL -m 5 -o /dev/null -X POST -H 'Content-Type: application/json' \
        -d "{\"chip\":\"${chip:-$machine}\"}" "$API/builds/missing" 2>/dev/null || true
    die 13
fi

tmp="/tmp/cerberus.$$"
rc="/tmp/S99cerberus.$$"
trap 'rm -f "$tmp" "$tmp.sums" "$rc"' EXIT INT TERM

curl -fsL -o "$tmp" "$DIST/cerberus-$arch" || die 14
curl -fsL -o "$rc" "$DIST/S99cerberus" || die 14

if command -v sha256sum >/dev/null 2>&1; then
    curl -fsL -o "$tmp.sums" "$DIST/SHA256SUMS" || die 15
    for pair in "$tmp:cerberus-$arch" "$rc:S99cerberus"; do
        file=${pair%%:*}
        name=${pair#*:}
        want=$(awk -v n="$name" '$2 == n || $2 == "*" n { print $1 }' "$tmp.sums")
        have=$(sha256sum "$file" | cut -d' ' -f1)
        [ -n "$want" ] && [ "$want" = "$have" ] || die 15
    done
    say "cerberus-$arch sha256 ok"
fi

stop_agent

chmod 0755 "$tmp" "$rc" || die 16
mv "$tmp" "$AGENT" || die 16
mv "$rc" "$INIT" || die 16
mkdir -p "$CONF" || die 16
chmod 0700 "$CONF" || die 16
say "installed $AGENT"

if [ -z "$TOKEN" ]; then
    say "E20"
    say "$DOCS"
    echo "    fw_setenv cerberus_api '$API'"
    echo "    fw_setenv cerberus_enrollment_token 'tok_...'"
    echo "    $INIT restart"
    exit 0
fi

fw_setenv cerberus_api "$API" || die 17
fw_setenv cerberus_enrollment_token "$TOKEN" || die 17

"$INIT" restart >/dev/null 2>&1 || die 18
say "started"

claim=""
i=0
while [ "$i" -lt 40 ]; do
    st=$(CERBERUS_PING_INTERVAL=0 "$AGENT" status 2>/dev/null || true)
    node=$(echo "$st" | awk '$1 == "node" { print $2 }')
    claim=$(echo "$st" | awk '$1 == "claim" && $2 == "code" { print $3 }')
    if [ -n "$node" ] && [ "$node" != "(not" ] && [ -n "$claim" ] && [ "$claim" != "-" ]; then
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
