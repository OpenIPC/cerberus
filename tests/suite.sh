#!/bin/sh
# The cerberus test suite. Runs inside a throwaway container (tests/run.sh starts it), as root:
# builds the agent, stands up a fake coordinator over real TLS, and walks everything the agent does
# without needing a cloud — identity, TLS verification, enrolment, the heartbeat back-channel,
# supervision — and then, where the kernel allows, a real WireGuard tunnel to a hub in its own netns.
#
# Most of it runs with STREQ_DRY_RUN=1: the agent prints the `ip`/`wg` commands it would run, and the
# suite checks those. The data-plane section at the end runs them for real.
#
# Env: REQUIRE_WG=1   fail (rather than skip) when kernel WireGuard or netns are unavailable — CI.
set -u

PASS=0
FAIL=0
SKIP=0

check() { # check <label> <actual> <expected>
	if [ "$2" = "$3" ]; then
		printf '  ok    %-52s %s\n' "$1" "$2"
		PASS=$((PASS + 1))
	else
		printf '  FAIL  %-52s got [%s], want [%s]\n' "$1" "$2" "$3"
		FAIL=$((FAIL + 1))
	fi
}
yes_if() { if "$@" >/dev/null 2>&1; then echo yes; else echo no; fi; }
skip() { printf '  skip  %s\n' "$1"; SKIP=$((SKIP + 1)); }
section() { echo; echo "== $1 =="; }
dump() { sed 's/^/        | /' "$1"; }

# ------------------------------------------------------------------------------- toolchain
echo "==> installing the toolchain"
if command -v apk >/dev/null 2>&1; then
	apk add --no-cache gcc musl-dev make mbedtls-dev mbedtls-static wireguard-tools iproute2 \
		openssl python3 curl >/dev/null 2>&1 || { echo "apk add failed"; exit 1; }
	LIBC=musl
else
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev make libmbedtls-dev \
		wireguard-tools iproute2 iputils-ping openssl python3 curl procps >/dev/null 2>&1 \
		|| { echo "apt-get install failed"; exit 1; }
	LIBC=glibc
fi

mkdir -p /build && cp /src/main.c /src/Makefile /build/ && cd /build || exit 1
SRC_VERSION=$(sed -n 's/^#define CERBERUS_VERSION "\(.*\)"$/\1/p' main.c)
MBEDTLS_VERSION=$(sed -n 's/^#define MBEDTLS_VERSION_STRING *"\(.*\)"$/\1/p' /usr/include/mbedtls/build_info.h /usr/include/mbedtls/version.h 2>/dev/null | head -1)
echo "    $LIBC, mbedTLS $MBEDTLS_VERSION, cerberus $SRC_VERSION"

section "build"
check "builds with -Werror" "$(yes_if make EXTRA_CFLAGS=-Werror)" "yes"
if [ "$LIBC" = musl ]; then
	cp cerberus /tmp/cerberus.dyn
	check "static build with -Werror" "$(yes_if sh -c 'make clean && make STATIC=1 EXTRA_CFLAGS=-Werror')" "yes"
	printf '  size  %-52s %s bytes\n' "static" "$(wc -c <cerberus)"
	cp /tmp/cerberus.dyn cerberus
fi
printf '  size  %-52s %s bytes\n' "dynamic" "$(wc -c <cerberus)"

make clean >/dev/null && make VERSION=9.9.9-test >/dev/null 2>&1
check "VERSION= is compiled in" "$(./cerberus --help 2>&1 | head -1 | cut -d' ' -f2)" "9.9.9-test"
make clean >/dev/null && make >/dev/null 2>&1 || { echo "build failed"; exit 1; }
check "--help exits 0" "$(./cerberus --help >/dev/null 2>&1; echo $?)" "0"
check "an unknown subcommand exits 2" "$(./cerberus frobnicate >/dev/null 2>&1; echo $?)" "2"

# ------------------------------------------------------------------------- fixtures
umask 077
T=/t
mkdir -p $T/bin && cd $T || exit 1

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 \
	-subj /CN=cerberus-test-ca -keyout ca.key -out ca.crt >/dev/null 2>&1
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
	-subj /CN=coordinator.test -keyout srv.key -out srv.csr >/dev/null 2>&1
printf 'subjectAltName=DNS:coordinator.test\n' >srv.ext
openssl x509 -req -in srv.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 2 \
	-extfile srv.ext -out srv.crt >/dev/null 2>&1
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 \
	-subj /CN=rogue-ca -keyout rogue.key -out rogue.crt >/dev/null 2>&1

wg genkey >hub.key
HUB_PUB=$(wg pubkey <hub.key)
HUB_EP=192.168.77.2:51820
OVERLAY=10.80.7.2
SUBNET=10.80.7.0/24
HUB_IP=10.80.0.1
TOKEN=tok_cerberus_test

python3 /src/tests/fake_coordinator.py --port 8443 --cert srv.crt --key srv.key \
	--token "$TOKEN" --overlay-ip $OVERLAY --network-subnet $SUBNET --hub-overlay-ip $HUB_IP \
	--hub-endpoint $HUB_EP --hub-public-key "$HUB_PUB" >coordinator.log 2>&1 &
COORD=$!
trap 'kill $COORD 2>/dev/null' EXIT

URL=https://coordinator.test:8443
c() { curl -s --cacert $T/ca.crt "$@"; }
ctl() { c -o /dev/null -X POST -H 'Content-Type: application/json' -d "$2" "$URL/_test/$1"; }
q() { c "$URL/_test/requests" | python3 -c "import sys, json; R = json.load(sys.stdin); $1"; }
sv() { sed -n "s/^$1=//p" "$2" | tail -1; }

for _ in 1 2 3 4 5 6 7 8 9 10; do c -o /dev/null "$URL/_test/requests" && break; sleep 0.5; done
c -o /dev/null "$URL/_test/requests" || { echo "the fake coordinator never came up"; dump coordinator.log; exit 1; }

A=/build/cerberus
export STREQ_API=$URL/v1
export STREQ_CA=$T/ca.crt
export STREQ_STORE=file:$T/agent.env
export STREQ_KEYFILE=$T/no-legacy-key
export STREQ_KEY_TMP=$T/.wg.key
export STREQ_HOSTNAME=cerberus-test
export STREQ_IFACE=cbr0
export STREQ_DRY_RUN=1
export STREQ_PING_INTERVAL=0

# ------------------------------------------------------------------------- identity
section "device identity"
$A keygen >keys 2>keygen.log
PRIV=$(sed -n 1p keys)
PUB=$(sed -n 2p keys)
check "X25519 pubkey matches \`wg pubkey\`" "$(printf '%s\n' "$PRIV" | wg pubkey)" "$PUB"
check "the private key lives in the store" "$(sv streq_device_key agent.env)" "$PRIV"
check "and nowhere on the rootfs" "$(yes_if test -e $T/no-legacy-key)" "no"
check "the store is 0600" "$(stat -c %a agent.env)" "600"
check "the key is stable across runs" "$($A keygen 2>/dev/null | sed -n 2p)" "$PUB"

wg genkey >legacy.key
STREQ_STORE=file:$T/legacy.env STREQ_KEYFILE=$T/legacy.key $A keygen >legacy.keys 2>legacy.log
check "a legacy key file is migrated in" "$(sv streq_device_key legacy.env)" "$(cat legacy.key)"
check "unchanged" "$(sed -n 2p legacy.keys)" "$(wg pubkey <legacy.key)"

printf 'streq_device_key=not-a-key\n' >broken.env
STREQ_STORE=file:$T/broken.env $A keygen >/dev/null 2>broken.log
check "a corrupt stored key is refused" "$(yes_if grep -q 'not a base64 WireGuard key' broken.log)" "yes"

# ------------------------------------------------------------------------------- TLS
section "TLS (mbedTLS $MBEDTLS_VERSION)"
STREQ_STORE=file:$T/rogue.env STREQ_CA=$T/rogue.crt STREQ_ENROLLMENT_TOKEN=$TOKEN $A enroll >rogue.log 2>&1
check "an unrelated CA is rejected" "$(yes_if grep -q 'TLS handshake .* failed' rogue.log)" "yes"
STREQ_STORE=file:$T/sni.env STREQ_API=https://127.0.0.1:8443/v1 STREQ_ENROLLMENT_TOKEN=$TOKEN $A enroll >sni.log 2>&1
check "a certificate for another name is rejected" "$(yes_if grep -q 'TLS handshake .* failed' sni.log)" "yes"
check "neither reached the coordinator" "$(q 'print(len(R))')" "0"
STREQ_STORE=file:$T/noca.env STREQ_CA=$T/missing.crt STREQ_ENROLLMENT_TOKEN=$TOKEN $A enroll >noca.log 2>&1
check "no trust store is fatal, and says so" "$(yes_if grep -q 'no usable CA trust store' noca.log)" "yes"
check "fatal errors point at the docs" "$(yes_if grep -q 'cerberus: https://' noca.log)" "yes"

# ------------------------------------------------------------------------ enrolment
section "enrolment"
$A enroll >notoken.log 2>&1
check "no token: refuses, non-zero" "$?" "1"
check "and says why" "$(yes_if grep -q 'no STREQ_ENROLLMENT_TOKEN' notoken.log)" "yes"
STREQ_ENROLLMENT_TOKEN=tok_wrong $A enroll >badtoken.log 2>&1
check "a wrong token is reported, non-zero" "$?" "1"
check "with the coordinator's status" "$(yes_if grep -q 'enroll rejected: HTTP 401' badtoken.log)" "yes"

STREQ_ENROLLMENT_TOKEN=$TOKEN $A enroll >enroll.log 2>&1
check "enrolls" "$?" "0"
check "over TLS" "$(yes_if grep -q '^cerberus: TLS ok' enroll.log)" "yes"
NODE=$(sv streq_node_id agent.env)
CLAIM=$(sv streq_claim_code agent.env)
check "node id stored" "$(echo "$NODE" | grep -c '^node_[0-9a-f]\{32\}$')" "1"
check "claim code: 12 chars of the unambiguous alphabet" \
	"$(yes_if sh -c "echo '$CLAIM' | grep -Eq '^[23456789ABCDEFGHJKMNPQRSTVWXYZ]{12}$'")" "yes"
check "claim code printed for the installer" "$(yes_if grep -q 'claim code: ' enroll.log)" "yes"

E='E = [r for r in R if r["path"] == "/v1/enroll"][-1]; B = E["body"]'
check "sends its public key" "$(q "$E; print(B['devicePublicKey'])")" "$PUB"
check "sends sha256(claim code), never the code" \
	"$(q "$E; print(B['claimCodeHash'])")" "$(printf '%s' "$CLAIM" | sha256sum | cut -d' ' -f1)"
check "the code itself never leaves the camera" "$(q "print(any('$CLAIM' in json.dumps(r) for r in R))")" "False"
check "kind" "$(q "$E; print(B['kind'])")" "Camera"
check "hostname" "$(q "$E; print(B['hostname'])")" "cerberus-test"
check "agentVersion" "$(q "$E; print(B['agentVersion'])")" "$SRC_VERSION"
check "User-Agent" "$(q "$E; print(E['userAgent'])")" "cerberus/$SRC_VERSION"

check "overlay ip stored" "$(sv streq_overlay_ip agent.env)" "$OVERLAY"
check "hub endpoint stored" "$(sv streq_hub_endpoint agent.env)" "$HUB_EP"
check "hub public key stored" "$(sv streq_hub_pubkey agent.env)" "$HUB_PUB"
check "network subnet stored" "$(sv streq_network_subnet agent.env)" "$SUBNET"
check "hub overlay ip stored" "$(sv streq_hub_ip agent.env)" "$HUB_IP"
check "store still 0600" "$(stat -c %a agent.env)" "600"

BEFORE=$(q 'print(len(R))')
STREQ_ENROLLMENT_TOKEN=$TOKEN $A enroll >again.log 2>&1
check "a second enroll is a no-op" "$(yes_if grep -q 'already enrolled' again.log)" "yes"
check "and spends nothing" "$(q 'print(len(R))')" "$BEFORE"
cp agent.env enrolled.env

STREQ_ENROLLMENT_TOKEN=$TOKEN $A status >status.out 2>/dev/null
check "status names the node" "$(yes_if grep -q "^node  *$NODE" status.out)" "yes"
check "status shows the claim code" "$(yes_if grep -q "^claim code  *$CLAIM" status.out)" "yes"

# -------------------------------------------------------------------- interface (dry)
section "interface programming (dry run)"
$A up >up.log 2>&1
check "up succeeds" "$?" "0"
check "wg set: key, hub, both allowed-ips, keepalive" "$(yes_if grep -qF \
	"dry-run: wg set cbr0 private-key $T/.wg.key peer $HUB_PUB endpoint $HUB_EP allowed-ips $SUBNET,$HUB_IP/32 persistent-keepalive 25" up.log)" "yes"
check "address with the tenant's prefix" "$(yes_if grep -qF "dry-run: ip address replace $OVERLAY/24 dev cbr0" up.log)" "yes"
check "a route to a hub outside the subnet" "$(yes_if grep -qF "dry-run: ip route replace $HUB_IP/32 dev cbr0" up.log)" "yes"
check "the key file does not outlive the call" "$(yes_if test -e $T/.wg.key)" "no"

# ------------------------------------------------------------------- back-channel
section "heartbeat and the back-channel"
runfor() { # runfor <seconds> <log> [env...] — `cerberus run` for a while, then SIGTERM
	_t=$1; _log=$2; shift 2
	env STREQ_HEARTBEAT=5 "$@" timeout "$_t" $A run >"$_log" 2>&1
}

cp enrolled.env agent.env
BEATS=$(q 'print(sum(r["path"].endswith("/heartbeat") for r in R))')
runfor 7 beat.log
check "heartbeats reach the coordinator" \
	"$(q "print(sum(r['path'] == '/v1/nodes/$NODE/heartbeat' for r in R) > $BEATS)")" "True"
check "carrying agentVersion" \
	"$(q "print([r for r in R if r['path'].endswith('/heartbeat')][-1]['body']['agentVersion'])")" "$SRC_VERSION"
check "SIGTERM stops it cleanly" "$(yes_if grep -q 'cerberus: stopping' beat.log)" "yes"
check "no rotation when nothing changed" "$(yes_if grep -q 'rotated' beat.log)" "no"

ctl command '{"command":"Restart"}'
runfor 8 restart.log
check "Restart: the agent re-execs itself" "$(yes_if grep -q 'cerberus: restarting' restart.log)" "yes"
check "and comes back watching" "$(grep -c 'cerberus: watching' restart.log)" "2"
check "a command is obeyed once" "$(grep -c 'cerberus: restarting' restart.log)" "1"

printf '#!/bin/sh\ntouch %s/rebooted\n' $T >$T/bin/reboot && chmod +x $T/bin/reboot
ctl command '{"command":"Reboot"}'
runfor 7 reboot.log PATH="$T/bin:$PATH"
check "Reboot: syncs and calls reboot" "$(yes_if test -e $T/rebooted)" "yes"

ctl command '{"command":"SelfDestruct"}'
runfor 7 unknown.log
check "an unknown command is ignored, loudly" "$(yes_if grep -q "unknown command 'SelfDestruct'" unknown.log)" "yes"

ctl join '{"hubEndpoint":"192.0.2.9:51820"}'
runfor 7 rot.log
check "a moved hub is noticed" "$(yes_if grep -q "hub endpoint rotated: $HUB_EP -> 192.0.2.9:51820" rot.log)" "yes"
check "persisted" "$(sv streq_hub_endpoint agent.env)" "192.0.2.9:51820"
check "and the interface reprogrammed" "$(yes_if grep -q 'flush dev cbr0' rot.log)" "yes"
NEWHUB=$(wg genkey | wg pubkey)
ctl join "{\"hubEndpoint\":\"$HUB_EP\",\"hubPublicKey\":\"$NEWHUB\",\"overlayIp\":\"10.80.7.9\"}"
runfor 7 rekey.log
check "a re-keyed hub is noticed" "$(yes_if grep -q 'hub public key rotated' rekey.log)" "yes"
check "persisted" "$(sv streq_hub_pubkey agent.env)" "$NEWHUB"
check "a new overlay address replaces the old" "$(yes_if grep -qF 'ip address replace 10.80.7.9/24 dev cbr0' rekey.log)" "yes"
ctl join "{\"hubPublicKey\":\"$HUB_PUB\",\"overlayIp\":\"$OVERLAY\"}"
cp enrolled.env agent.env

ctl fail '{"status":503}'
( sleep 6; ctl fail '{"status":0}' ) &
runfor 14 outage.log
check "a coordinator outage is one warning" "$(grep -c 'heartbeat to .* failed' outage.log)" "1"
check "and survived" "$(yes_if grep -q 'heartbeat recovered' outage.log)" "yes"

# ---------------------------------------------------------------- waiting it out
section "an unreachable coordinator"
W0=$(date +%s)
STREQ_STORE=file:$T/wait.env STREQ_API=https://127.0.0.1:9/v1 STREQ_ENROLLMENT_TOKEN=$TOKEN \
	STREQ_RETRY_MIN=2 timeout 9 $A run >wait.log 2>&1
check "is waited out, not fatal" "$([ $(( $(date +%s) - W0 )) -ge 8 ] && echo yes || echo no)" "yes"
check "with a backoff between tries" "$(yes_if grep -q 'retrying in 4s' wait.log)" "yes"

STREQ_STORE=file:$T/wait2.env STREQ_API=https://127.0.0.1:9/v1 STREQ_ENROLLMENT_TOKEN=$TOKEN \
	STREQ_RETRY_MIN=30 $A run >wait2.log 2>&1 &
WPID=$!
sleep 3
S0=$(date +%s)
kill -TERM $WPID
wait $WPID
check "SIGTERM during the pause exits 0" "$?" "0"
check "at once" "$(( $(date +%s) - S0 ))" "0"

# ------------------------------------------------------------------- data plane
section "the data plane: a real tunnel to a hub in its own netns"
wg_ok() {
	ip link add cbr-probe type wireguard 2>/dev/null && ip link del cbr-probe \
		&& ip netns add hub 2>/dev/null
}
if wg_ok; then
	ip link add veth-cam type veth peer name veth-hub
	ip link set veth-hub netns hub
	ip addr add 192.168.77.1/24 dev veth-cam && ip link set veth-cam up
	ip -n hub addr add 192.168.77.2/24 dev veth-hub
	ip -n hub link set veth-hub up && ip -n hub link set lo up
	ip netns exec hub ip link add wg-hub type wireguard
	ip netns exec hub wg set wg-hub private-key $T/hub.key listen-port 51820 \
		peer "$PUB" allowed-ips $OVERLAY/32
	ip -n hub addr add $HUB_IP/32 dev wg-hub && ip -n hub link set wg-hub up
	ip -n hub route add $SUBNET dev wg-hub

	cp enrolled.env agent.env
	export STREQ_DRY_RUN=0 STREQ_IFACE=wg0
	$A up >dp-up.log 2>&1
	check "wg0 comes up with the overlay address" \
		"$(yes_if sh -c "ip -4 -o addr show wg0 | grep -q ' $OVERLAY/24 '")" "yes"
	check "with a route to the hub" "$(yes_if sh -c "ip route show $HUB_IP | grep -q 'dev wg0'")" "yes"

	hs=no
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		ping -c1 -W1 $HUB_IP >/dev/null 2>&1 || true
		[ -n "$(wg show wg0 latest-handshakes | cut -f2 | grep -v '^0$')" ] && { hs=yes; break; }
		sleep 1
	done
	check "handshake with the hub" "$hs" "yes"
	check "ping $HUB_IP through the tunnel" "$(yes_if ping -c3 -W2 $HUB_IP)" "yes"

	STREQ_PING_INTERVAL=60 $A status >dp-status.out 2>/dev/null
	check "status reads the handshake back" "$(yes_if grep -Eq '^handshake +[0-9]+s ago' dp-status.out)" "yes"
	check "and probes the tunnel itself" "$(yes_if grep -q "^tunnel ping    ok ($HUB_IP)" dp-status.out)" "yes"

	# A tunnel that is up on paper and dead in fact: the handshake stays fresh, the route is gone.
	# Only the in-tunnel probe can see it, and the agent has to repair it by itself.
	env STREQ_HEARTBEAT=0 STREQ_PING_INTERVAL=10 STREQ_PING_FAILS=1 timeout 26 $A run >dp-run.log 2>&1 &
	RPID=$!
	sleep 3
	ip route del $HUB_IP/32 dev wg0
	wait $RPID
	check "a stolen route is noticed" "$(yes_if grep -q "no reply from $HUB_IP inside the tunnel" dp-run.log)" "yes"
	check "the interface is rebuilt" "$(yes_if grep -q 'rebuilding wg0 from scratch' dp-run.log)" "yes"
	check "and the tunnel answers again" "$(yes_if grep -q 'the tunnel answers again' dp-run.log)" "yes"
	check "route restored" "$(yes_if sh -c "ip route show $HUB_IP | grep -q 'dev wg0'")" "yes"

	ip link del wg0 2>/dev/null
	export STREQ_DRY_RUN=1 STREQ_IFACE=cbr0
else
	if [ "${REQUIRE_WG:-}" = 1 ]; then
		check "kernel WireGuard and netns available" "no" "yes"
	else
		skip "no kernel WireGuard or no netns here (run the container --privileged on a WireGuard kernel)"
	fi
fi

# ------------------------------------------------------------------ revocation
section "revocation and removal"
cp enrolled.env agent.env
ctl forget '{}'
F0=$(date +%s)
runfor 20 gone.log
check "a forgotten node discards its enrolment" "$(yes_if grep -q 'enrolment discarded' gone.log)" "yes"
check "and stops by itself" "$([ $(( $(date +%s) - F0 )) -lt 15 ] && echo yes || echo no)" "yes"
check "node id cleared" "$(sv streq_node_id agent.env)" ""
check "hub endpoint cleared" "$(sv streq_hub_endpoint agent.env)" ""
check "device key kept" "$(sv streq_device_key agent.env)" "$PRIV"
check "claim code kept" "$(sv streq_claim_code agent.env)" "$CLAIM"

mkdir -p /etc/init.d $T/victim
cp $A $T/victim/cerberus && touch /etc/init.d/S99cerberus
cp enrolled.env clean.env
STREQ_STORE=file:$T/clean.env $T/victim/cerberus clean >clean.log 2>&1
check "clean: removes the init script" "$(yes_if test -e /etc/init.d/S99cerberus)" "no"
check "the stored material" "$(yes_if test -e $T/clean.env)" "no"
sleep 4
check "and, a moment later, itself" "$(yes_if test -e $T/victim/cerberus)" "no"

# ------------------------------------------------------------------ installers
section "shipped scripts"
for s in S99cerberus bootstrap.sh uninstall.sh; do
	check "$s parses" "$(yes_if sh -n /src/$s)" "yes"
done
for s in bootstrap.sh uninstall.sh; do
	check "$s carries no comments" "$(tail -n +2 /src/$s | grep -c '^[[:space:]]*#')" "0"
done

echo
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
