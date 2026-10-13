#!/bin/sh
set -eu

DOCS="${STREQ_DOCS:-https://cloud.openipc.ru/docs/openipc-camera/#codes}"
AGENT=/usr/sbin/cerberus
INIT=/etc/init.d/S99cerberus

say() { echo "cerberus: $*"; }
die() { echo "cerberus: E$1" >&2; echo "cerberus: $DOCS" >&2; exit "$1"; }

[ "$(id -u)" = 0 ] || die 10
command -v fw_setenv >/dev/null 2>&1 || die 11

if [ -x "$INIT" ]; then
    "$INIT" stop >/dev/null 2>&1 || true
fi

iface=$(fw_printenv -n streq_iface 2>/dev/null || true)
ip link del dev "${iface:-wg0}" >/dev/null 2>&1 || true

for key in streq_node_id streq_overlay_ip streq_hub_endpoint streq_hub_pubkey \
           streq_allowed_ips streq_network_subnet streq_hub_ip streq_claim_code \
           streq_device_key streq_api streq_enrollment_token streq_hostname \
           streq_kind streq_iface streq_subnet streq_ca streq_heartbeat \
           streq_ping_target streq_ping_interval streq_ping_timeout streq_ping_fails; do
    fw_setenv "$key" >/dev/null 2>&1 || die 30
done

rm -f "$INIT" || die 30
rm -f "$AGENT" || die 30
rm -rf /etc/streq || die 30
say "removed"
