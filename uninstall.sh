#!/bin/sh
set -eu

DOCS="${CERBERUS_DOCS:-${STREQ_DOCS:-https://cloud.openipc.ru/docs/openipc-camera/#codes}}"
AGENT=/usr/sbin/cerberus
INIT=/etc/init.d/S99cerberus

say() { echo "cerberus: $*"; }
die() { echo "cerberus: E$1" >&2; echo "cerberus: $DOCS" >&2; exit "$1"; }

[ "$(id -u)" = 0 ] || die 10
command -v fw_setenv >/dev/null 2>&1 || die 11

if [ -x "$INIT" ]; then
    "$INIT" stop >/dev/null 2>&1 || true
fi

iface=$(fw_printenv -n cerberus_iface 2>/dev/null || fw_printenv -n streq_iface 2>/dev/null || true)
ip link del dev "${iface:-wg0}" >/dev/null 2>&1 || true

for name in node_id overlay_ip hub_endpoint hub_pubkey allowed_ips network_subnet hub_ip \
            claim_code device_key api enrollment_token hostname kind iface subnet ca heartbeat \
            ping_target ping_interval ping_timeout ping_fails; do
    for prefix in cerberus streq; do
        if fw_printenv -n "${prefix}_$name" >/dev/null 2>&1; then
            fw_setenv "${prefix}_$name" >/dev/null 2>&1 || die 30
        fi
    done
done

rm -f "$INIT" || die 30
rm -f "$AGENT" || die 30
rm -rf /etc/cerberus /etc/streq || die 30
say "removed"
