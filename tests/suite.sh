#!/bin/sh
# The cerberus test suite. Runs inside a throwaway container (tests/run.sh starts it), as root:
# builds the agent, stands up a fake coordinator over real TLS, and walks everything the agent does
# without needing a cloud — identity, TLS verification, enrolment, the heartbeat back-channel,
# supervision — and then, where the kernel allows, a real WireGuard tunnel to a hub in its own netns.
#
# Most of it runs with CERBERUS_DRY_RUN=1: the agent prints the `ip`/`wg` commands it would run, and the
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
daemon_of() { sh -c 'eval "$(sed -n "/^DAEMON=/p")"; printf %s "$DAEMON"'; }
skip() { printf '  skip  %s\n' "$1"; SKIP=$((SKIP + 1)); }
section() { echo; echo "== $1 =="; }
dump() { sed 's/^/        | /' "$1"; }

# ------------------------------------------------------------------------------- toolchain
echo "==> installing the toolchain"
if command -v apk >/dev/null 2>&1; then
	# openrc only for start-stop-daemon, which the init script calls: BusyBox on a camera has it, Alpine's does not.
	apk add --no-cache gcc musl-dev make mbedtls-dev mbedtls-static wireguard-tools iproute2 \
		openssl python3 curl openrc >/dev/null 2>&1 || { echo "apk add failed"; exit 1; }
	# The init script is text inside the binary, so this is the only place it gets linted.
	apk add --no-cache shellcheck >/dev/null 2>&1 || true
	LIBC=musl
else
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev make libmbedtls-dev \
		wireguard-tools iproute2 iputils-ping openssl python3 curl procps >/dev/null 2>&1 \
		|| { echo "apt-get install failed"; exit 1; }
	LIBC=glibc
fi

mkdir -p /build && cp /src/src/cerberus.c /src/src/Makefile /build/ && cd /build || exit 1
SRC_VERSION=$(sed -n 's/^#define CERBERUS_VERSION "\(.*\)"$/\1/p' cerberus.c)
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
check "--help lists every knob (#10)" \
	"$(./cerberus --help 2>&1 | grep -cE 'CERBERUS_(STALE_AFTER|RETRY_MIN|RETRY_MAX|MOTION_MAX) ')" "4"

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

# A U-Boot environment in a file, behind the same two commands a camera has. `fw_setenv k` with
# no value removes k, as the real one does; every write is counted, because on a camera each one
# is a flash erase.
mkdir -p $T/fw
cat >$T/fw/fw_printenv <<'EOF'
#!/bin/sh
f=${FAKE_UBOOT:?}
touch "$f"
if [ "${1:-}" = -n ]; then
	awk -v k="$2" 'index($0, k "=") == 1 { print substr($0, length(k) + 2); found = 1 } END { exit !found }' "$f" \
		|| { echo "## Error: \"$2\" not defined" >&2; exit 1; }
	exit 0
fi
cat "$f"
EOF
cat >$T/fw/fw_setenv <<'EOF'
#!/bin/sh
f=${FAKE_UBOOT:?}
touch "$f"
k=$1
shift
awk -v k="$k" 'index($0, k "=") != 1' "$f" >"$f.tmp"
[ $# -gt 0 ] && printf '%s=%s\n' "$k" "$*" >>"$f.tmp"
mv "$f.tmp" "$f"
echo "$k" >>"$f.writes"
EOF
chmod +x $T/fw/fw_printenv $T/fw/fw_setenv
uboot() { _u=$1; shift; env PATH="$T/fw:$PATH" FAKE_UBOOT="$_u" CERBERUS_STORE=uboot "$@"; }
ub() { FAKE_UBOOT="$1" $T/fw/fw_printenv -n "$2" 2>/dev/null; }
writes() { if [ -f "$1.writes" ]; then wc -l <"$1.writes" | tr -d ' '; else echo 0; fi; }

wg genkey >hub.key
HUB_PUB=$(wg pubkey <hub.key)
HUB_EP=192.168.77.2:51820
OVERLAY=10.80.7.2
SUBNET=10.80.7.0/24
HUB_IP=10.80.0.1
TOKEN=tok_cerberus_test

python3 /src/tests/fake_coordinator.py --port 8443 --cert srv.crt --key srv.key \
	--token "$TOKEN" --token-max-uses 2 --overlay-ip $OVERLAY --network-subnet $SUBNET --hub-overlay-ip $HUB_IP \
	--hub-endpoint $HUB_EP --hub-public-key "$HUB_PUB" --majestic-port 8081 >coordinator.log 2>&1 &
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
export CERBERUS_API=$URL/v1
export CERBERUS_CA=$T/ca.crt
export CERBERUS_STORE=file:$T/agent.env
export CERBERUS_KEYFILE=$T/no-legacy-key
export CERBERUS_KEY_TMP=$T/.wg.key
export CERBERUS_HOSTNAME=cerberus-test
export CERBERUS_IFACE=cbr0
export CERBERUS_DRY_RUN=1
export CERBERUS_PING_INTERVAL=0
export CERBERUS_SEED=$T/seed

# ------------------------------------------------------------------------- identity
section "device identity"
$A keygen >keys 2>keygen.log
PRIV=$(sed -n 1p keys)
PUB=$(sed -n 2p keys)
check "X25519 pubkey matches \`wg pubkey\`" "$(printf '%s\n' "$PRIV" | wg pubkey)" "$PUB"
check "the private key lives in the store" "$(sv cerberus_device_key agent.env)" "$PRIV"
check "and nowhere on the rootfs" "$(yes_if test -e $T/no-legacy-key)" "no"
check "the store is 0600" "$(stat -c %a agent.env)" "600"
check "the key is stable across runs" "$($A keygen 2>/dev/null | sed -n 2p)" "$PUB"

wg genkey >legacy.key
CERBERUS_STORE=file:$T/legacy.env CERBERUS_KEYFILE=$T/legacy.key $A keygen >legacy.keys 2>legacy.log
check "a legacy key file is migrated in" "$(sv cerberus_device_key legacy.env)" "$(cat legacy.key)"
check "unchanged" "$(sed -n 2p legacy.keys)" "$(wg pubkey <legacy.key)"

printf 'cerberus_device_key=not-a-key\n' >broken.env
CERBERUS_STORE=file:$T/broken.env $A keygen >/dev/null 2>broken.log
check "a corrupt stored key is refused" "$(yes_if grep -q 'not a base64 WireGuard key' broken.log)" "yes"

# ------------------------------------------------------------------------ randomness
# A camera whose kernel held every start for minutes while mbedTLS gathered entropy. Its stand-in is
# a FIFO nobody writes to: opened non-blocking it reads empty at once, opened the ordinary way it
# waits forever. getrandom(2) is switched off so the kernel's real pool cannot step in.
section "randomness: a start never waits on the kernel"
mkfifo $T/dry-random
dry() { env CERBERUS_GETRANDOM=0 CERBERUS_RANDOM_DEV=$T/dry-random timeout "$@"; }

check "a start keeps a seed for the next one" "$(stat -c %a $T/seed 2>/dev/null)" "600"
check "32 bytes of it" "$(wc -c <$T/seed | tr -d ' ')" "32"
check "status needs no randomness at all" "$(dry 5 $A status >/dev/null 2>&1; echo $?)" "0"
SEED_BEFORE=$(od -An -tx1 $T/seed | tr -d ' \n')
check "with a seed and a dry kernel: no wait" "$(dry 10 $A keygen 2>/dev/null | sed -n 2p)" "$PUB"
check "and the seed is replaced" "$([ "$(od -An -tx1 $T/seed | tr -d ' \n')" != "$SEED_BEFORE" ] && echo yes || echo no)" "yes"
CERBERUS_SEED=$T/no-seed dry 3 $A keygen >/dev/null 2>dry.log
check "no seed, dry kernel: waits rather than invent any" "$(yes_if grep -q "waiting for the kernel's randomness" dry.log)" "yes"
check "and gives nothing out meanwhile" "$(yes_if test -e $T/no-seed)" "no"

# ------------------------------------------------------------------------------- TLS
section "TLS (mbedTLS $MBEDTLS_VERSION)"
CERBERUS_STORE=file:$T/rogue.env CERBERUS_CA=$T/rogue.crt CERBERUS_ENROLLMENT_TOKEN=$TOKEN $A enroll >rogue.log 2>&1
check "an unrelated CA is rejected" "$(yes_if grep -q 'TLS handshake .* failed' rogue.log)" "yes"
CERBERUS_STORE=file:$T/sni.env CERBERUS_API=https://127.0.0.1:8443/v1 CERBERUS_ENROLLMENT_TOKEN=$TOKEN $A enroll >sni.log 2>&1
check "a certificate for another name is rejected" "$(yes_if grep -q 'TLS handshake .* failed' sni.log)" "yes"
check "neither reached the coordinator" "$(q 'print(len(R))')" "0"
CERBERUS_STORE=file:$T/noca.env CERBERUS_CA=$T/missing.crt CERBERUS_ENROLLMENT_TOKEN=$TOKEN $A enroll >noca.log 2>&1
check "no trust store is fatal, and says so" "$(yes_if grep -q 'no usable CA trust store' noca.log)" "yes"
check "fatal errors point at the docs" "$(yes_if grep -q 'cerberus: https://' noca.log)" "yes"

# ------------------------------------------------------------------------ enrolment
section "enrolment"
$A enroll >notoken.log 2>&1
check "no token: refuses, non-zero" "$?" "1"
check "and says why" "$(yes_if grep -q 'no CERBERUS_ENROLLMENT_TOKEN' notoken.log)" "yes"
CERBERUS_ENROLLMENT_TOKEN=tok_wrong $A enroll >badtoken.log 2>&1
check "a wrong token is reported, non-zero" "$?" "1"
check "with the coordinator's status" "$(yes_if grep -q 'enroll rejected: HTTP 401' badtoken.log)" "yes"

# A hostname is typed by a person (#8): quotes, a backslash and a tab must arrive as JSON, as typed.
# A wrong token keeps the real one unspent; the body is recorded as parsed JSON only when it is JSON.
export ODD_HOST='Gate "north" \ 1	x'
CERBERUS_STORE=file:$T/odd.env CERBERUS_HOSTNAME="$ODD_HOST" CERBERUS_ENROLLMENT_TOKEN=tok_wrong $A enroll >odd.log 2>&1
check "a hostname with quotes is sent as valid JSON" \
	"$(q 'import os; E = [r for r in R if r["path"] == "/v1/enroll"][-1]; print(E["body"]["hostname"] == os.environ["ODD_HOST"])')" "True"
BEFORE=$(q 'print(len(R))')
CERBERUS_STORE=file:$T/odd.env CERBERUS_HOSTNAME="$(head -c 3000 /dev/zero | tr '\0' h)" CERBERUS_ENROLLMENT_TOKEN=tok_wrong \
	$A enroll >long.log 2>&1
check "a request that does not fit is refused, non-zero" "$?" "1"
check "before anything is sent" "$(q 'print(len(R))')" "$BEFORE"
check "and says so" "$(yes_if grep -q 'does not fit' long.log)" "yes"

# A malformed address costs what depends on it, never the running agent (#6).
CERBERUS_API=http:// $A run >badapi.log 2>&1
check "run: CERBERUS_API without a host is refused at start" "$?:$(yes_if grep -q 'CERBERUS_API is not an http(s) URL' badapi.log)" "1:yes"
for u in 127.0.0.1/image.jpg http:// http://user@/image.jpg; do
	CERBERUS_SNAPSHOT_URL=$u $A snapshot >badsnap.log 2>&1
	check "snapshot from '$u': a warning, not a fatal error" \
		"$(yes_if grep -q 'not an http(s) URL' badsnap.log)$(yes_if grep -q 'cerberus: error' badsnap.log)" "yesno"
done

CERBERUS_ENROLLMENT_TOKEN=$TOKEN $A enroll >enroll.log 2>&1
check "enrolls" "$?" "0"
check "over TLS" "$(yes_if grep -q '^cerberus: TLS ok' enroll.log)" "yes"
NODE=$(sv cerberus_node_id agent.env)
CLAIM=$(sv cerberus_claim_code agent.env)
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

check "overlay ip stored" "$(sv cerberus_overlay_ip agent.env)" "$OVERLAY"
check "hub endpoint stored" "$(sv cerberus_hub_endpoint agent.env)" "$HUB_EP"
check "hub public key stored" "$(sv cerberus_hub_pubkey agent.env)" "$HUB_PUB"
check "network subnet stored" "$(sv cerberus_network_subnet agent.env)" "$SUBNET"
check "hub overlay ip stored" "$(sv cerberus_hub_ip agent.env)" "$HUB_IP"
check "store still 0600" "$(stat -c %a agent.env)" "600"

BEFORE=$(q 'print(len(R))')
CERBERUS_ENROLLMENT_TOKEN=$TOKEN $A enroll >again.log 2>&1
check "a second enroll is a no-op" "$(yes_if grep -q 'already enrolled' again.log)" "yes"
check "and spends nothing" "$(q 'print(len(R))')" "$BEFORE"
cp agent.env enrolled.env

CERBERUS_ENROLLMENT_TOKEN=$TOKEN $A status >status.out 2>/dev/null
check "status names the node" "$(yes_if grep -q "^node  *$NODE" status.out)" "yes"
check "status shows the claim code" "$(yes_if grep -q "^claim code  *$CLAIM" status.out)" "yes"

# The probe pings the hub in the network the interface is in (#7): with a networkSubnet and no
# hubOverlayIp, the first address of that subnet — not of CERBERUS_SUBNET's default pool.
sed '/^cerberus_hub_ip=/d' enrolled.env >nohub.env
check "ping target: the network's subnet, not CERBERUS_SUBNET" \
	"$(CERBERUS_STORE=file:$T/nohub.env CERBERUS_PING_INTERVAL=60 CERBERUS_PING_TIMEOUT=1 $A status 2>/dev/null | sed -n 's/^tunnel ping.*(\(.*\))$/\1/p')" \
	"$(echo $SUBNET | sed 's|0/24$|1|')"

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
	env CERBERUS_HEARTBEAT=5 "$@" timeout "$_t" $A run >"$_log" 2>&1
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
check "persisted" "$(sv cerberus_hub_endpoint agent.env)" "192.0.2.9:51820"
check "and the interface reprogrammed" "$(yes_if grep -q 'flush dev cbr0' rot.log)" "yes"
NEWHUB=$(wg genkey | wg pubkey)
ctl join "{\"hubEndpoint\":\"$HUB_EP\",\"hubPublicKey\":\"$NEWHUB\",\"overlayIp\":\"10.80.7.9\"}"
runfor 7 rekey.log
check "a re-keyed hub is noticed" "$(yes_if grep -q 'hub public key rotated' rekey.log)" "yes"
check "persisted" "$(sv cerberus_hub_pubkey agent.env)" "$NEWHUB"
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
CERBERUS_STORE=file:$T/wait.env CERBERUS_API=https://127.0.0.1:9/v1 CERBERUS_ENROLLMENT_TOKEN=$TOKEN \
	CERBERUS_RETRY_MIN=2 timeout 9 $A run >wait.log 2>&1
check "is waited out, not fatal" "$([ $(( $(date +%s) - W0 )) -ge 8 ] && echo yes || echo no)" "yes"
check "with a backoff between tries" "$(yes_if grep -q 'retrying in 4s' wait.log)" "yes"

CERBERUS_STORE=file:$T/wait2.env CERBERUS_API=https://127.0.0.1:9/v1 CERBERUS_ENROLLMENT_TOKEN=$TOKEN \
	CERBERUS_RETRY_MIN=30 $A run >wait2.log 2>&1 &
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
	export CERBERUS_DRY_RUN=0 CERBERUS_IFACE=wg0
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

	CERBERUS_PING_INTERVAL=60 $A status >dp-status.out 2>/dev/null
	check "status reads the handshake back" "$(yes_if grep -Eq '^handshake +[0-9]+s ago' dp-status.out)" "yes"
	check "and probes the tunnel itself" "$(yes_if grep -q "^tunnel ping    ok ($HUB_IP)" dp-status.out)" "yes"

	# A tunnel that is up on paper and dead in fact: the handshake stays fresh, the route is gone.
	# Only the in-tunnel probe can see it, and the agent has to repair it by itself.
	env CERBERUS_HEARTBEAT=0 CERBERUS_PING_INTERVAL=10 CERBERUS_PING_FAILS=1 timeout 26 $A run >dp-run.log 2>&1 &
	RPID=$!
	sleep 3
	ip route del $HUB_IP/32 dev wg0
	wait $RPID
	check "a stolen route is noticed" "$(yes_if grep -q "no reply from $HUB_IP inside the tunnel" dp-run.log)" "yes"
	check "the interface is rebuilt" "$(yes_if grep -q 'rebuilding wg0 from scratch' dp-run.log)" "yes"
	check "and the tunnel answers again" "$(yes_if grep -q 'the tunnel answers again' dp-run.log)" "yes"
	check "route restored" "$(yes_if sh -c "ip route show $HUB_IP | grep -q 'dev wg0'")" "yes"

	ip link del wg0 2>/dev/null
	export CERBERUS_DRY_RUN=1 CERBERUS_IFACE=cbr0
else
	if [ "${REQUIRE_WG:-}" = 1 ]; then
		check "kernel WireGuard and netns available" "no" "yes"
	else
		skip "no kernel WireGuard or no netns here (run the container --privileged on a WireGuard kernel)"
	fi
fi

# -------------------------------------------------------------- the old names
section "the rename: streq_* keys and STREQ_* variables"
# A camera installed before the rename: its whole identity under the old names, in U-Boot env.
OLD=$T/old-uboot.env
sed -n 's/^cerberus_\(.*\)=\(.*\)$/streq_\1=\2/p' enrolled.env >$OLD
uboot $OLD $A status >mig.out 2>mig.log
check "moves its identity to cerberus_*" "$(ub $OLD cerberus_device_key)" "$PRIV"
check "and its enrolment" "$(ub $OLD cerberus_node_id)" "$NODE"
check "every agent-written key" "$(yes_if grep -q 'moved 9 key(s)' mig.log)" "yes"
check "keeps the streq_* copies for an older init script" "$(ub $OLD streq_node_id)" "$NODE"
check "the camera is the same camera" "$(uboot $OLD $A keygen 2>/dev/null | sed -n 2p)" "$PUB"
check "and knows it is enrolled" "$(yes_if grep -q "^node  *$NODE" mig.out)" "yes"
W=$(writes $OLD)
uboot $OLD $A status >/dev/null 2>&1
uboot $OLD $A status >/dev/null 2>&1
check "after that, starts write nothing to flash" "$(writes $OLD)" "$W"

OLDF=$T/old-file.env
sed 's/^cerberus_/streq_/' enrolled.env >$OLDF
CERBERUS_STORE=file:$OLDF $A status >/dev/null 2>&1
check "a file store is moved the same way" "$(sv cerberus_node_id $OLDF)" "$NODE"

check "STREQ_* variables are still read" \
	"$( (unset CERBERUS_STORE; STREQ_STORE=file:$T/old-var.env $A keygen >/dev/null 2>&1); yes_if test -s $T/old-var.env)" "yes"
check "CERBERUS_* wins over STREQ_*" \
	"$( (STREQ_STORE=file:$T/loser.env $A keygen >/dev/null 2>&1); yes_if test -e $T/loser.env)" "no"

# The init script reads the new name, and the old one when the new one is not there.
SUB=$T/s99.env
S99=$T/S99cerberus
printf 'streq_subnet=10.99.0.0/16\n' >$SUB
$A init-script $S99 2>/dev/null
check "the init script falls back to streq_* keys" \
	"$(env PATH="$T/fw:$PATH" FAKE_UBOOT=$SUB CERBERUS_STORE=file:$T/nothing.env sh $S99 status 2>/dev/null | sed -n 's/^network subnet *//p')" \
	"10.99.0.0/16 (from CERBERUS_SUBNET)"
printf 'cerberus_subnet=10.98.0.0/16\n' >>$SUB
check "and prefers cerberus_*" \
	"$(env PATH="$T/fw:$PATH" FAKE_UBOOT=$SUB CERBERUS_STORE=file:$T/nothing.env sh $S99 status 2>/dev/null | sed -n 's/^network subnet *//p')" \
	"10.98.0.0/16 (from CERBERUS_SUBNET)"
check "unprovisioned: start says so and exits 0" \
	"$(env PATH="$T/fw:$PATH" FAKE_UBOOT=$T/empty.env sh $S99 start 2>&1; echo "rc=$?")" \
	"cerberus: not provisioned (fw_setenv cerberus_api ...)
rc=0"

# ------------------------------------------------------------------ the installer
section "the installer, from a release laid out on disk"
# What ci/assemble.sh publishes, with this machine's build standing in for the release binary.
REL=$T/release
mkdir -p $REL
ARCH=$(uname -m)
cp $A $REL/cerberus-$ARCH
cp /src/install.sh /src/uninstall.sh $REL/
(cd $REL && for f in cerberus-$ARCH install.sh uninstall.sh; do
	printf '%s  %s\n' "$(sha256sum $f | cut -d' ' -f1)" "$f"; done >SHA256SUMS)
rm -f /usr/sbin/cerberus /etc/init.d/S99cerberus
mkdir -p /etc/init.d

INST=$T/inst.env
printf 'cerberus_ca=%s\n' "$T/ca.crt" >$INST
inst() { env PATH="$T/fw:$PATH" FAKE_UBOOT=$INST CERBERUS_DIST=file://$REL CERBERUS_API=$URL/v1 \
	CERBERUS_STORE= CERBERUS_IFACE= sh /src/install.sh "$@"; }

# A flag it does not know is refused before anything happens: taken for a token, a mistyped
# --uninstall would be written into the environment and enrolled with.
inst --uninstal >inst-flag.log 2>&1
check "an unknown flag is refused as E19" "$?" "19"
check "before it installs anything" "$(yes_if test -e /usr/sbin/cerberus)$(yes_if test -e /etc/init.d/S99cerberus)" "nono"
check "or writes anything" "$(grep -c -E '^(cerberus|streq)_(api|enrollment_token)=' $INST)" "0"

inst >inst-notoken.log 2>&1
check "no token: installs, exits 0" "$?" "0"
check "picks the build for this CPU" "$(cmp -s /usr/sbin/cerberus $REL/cerberus-$ARCH && echo yes)" "yes"
check "verifies it against SHA256SUMS" "$(yes_if grep -q "cerberus-$ARCH sha256 ok" inst-notoken.log)" "yes"
check "installs the init script" "$(yes_if test -x /etc/init.d/S99cerberus)" "yes"
check "written by the agent it starts" "$(daemon_of </etc/init.d/S99cerberus)" "/usr/sbin/cerberus"
check "and says what to set, by the new names" "$(yes_if grep -q 'fw_setenv cerberus_api' inst-notoken.log)" "yes"

cp $REL/SHA256SUMS $T/sums.good
sed -i "s/^[0-9a-f]*  cerberus-$ARCH\$/$(printf '0%.0s' $(seq 64))  cerberus-$ARCH/" $REL/SHA256SUMS
inst >inst-bad.log 2>&1
check "a binary that does not match its sum is refused" "$?" "15"
cp $T/sums.good $REL/SHA256SUMS

# Where the init script cannot go, the installer stops with E16, as it always did.
mv /etc/init.d /etc/init.d.real && touch /etc/init.d
inst >inst-noinit.log 2>&1
check "an /etc/init.d it cannot write to is E16" "$?" "16"
rm -f /etc/init.d && mv /etc/init.d.real /etc/init.d

# A claim code exists before the camera ever reaches the cloud — its hash is what enrolment sends —
# so seeing one proves nothing. The installer has to wait for the enrolment itself, and say E21
# when it does not come.
env PATH="$T/fw:$PATH" FAKE_UBOOT=$INST CERBERUS_DIST=file://$REL CERBERUS_API=https://127.0.0.1:9/v1 \
	CERBERUS_STORE= CERBERUS_IFACE= sh /src/install.sh "$TOKEN" >inst-noreach.log 2>&1
check "an unreachable cloud is reported as E21" "$(yes_if grep -q 'cerberus: E21' inst-noreach.log)" "yes"
check "not as a claim code" "$(yes_if grep -q 'cerberus: claim' inst-noreach.log)" "no"
env PATH="$T/fw:$PATH" FAKE_UBOOT=$INST sh /etc/init.d/S99cerberus stop >/dev/null 2>&1
printf 'cerberus_ca=%s\n' "$T/ca.crt" >$INST

inst "$TOKEN" >inst.log 2>&1
check "with a token: installs, writes it, starts the agent" "$?" "0"
check "the token went to cerberus_enrollment_token" "$(ub $INST cerberus_enrollment_token)" "$TOKEN"
check "the agent enrolled and the installer read its claim code" "$(yes_if grep -Eq '^cerberus: claim [2-9A-Z]{12}$' inst.log)" "yes"
check "into the U-Boot environment" "$(ub $INST cerberus_node_id | grep -c '^node_')" "1"
env PATH="$T/fw:$PATH" FAKE_UBOOT=$INST sh /etc/init.d/S99cerberus stop >/dev/null 2>&1

inst --uninstall >uninst.log 2>&1
check "install.sh --uninstall removes it" "$(yes_if test -e /usr/sbin/cerberus)$(yes_if test -e /etc/init.d/S99cerberus)" "nono"
check "and every key it wrote" "$(grep -c -E '^(cerberus|streq)_' $INST)" "0"

# uninstall.sh is only the old address of the same thing: it fetches install.sh from the release and
# runs it with --uninstall, so a command printed long ago still works — old key names included.
inst >/dev/null 2>&1
printf 'cerberus_api=x\nstreq_node_id=node_old\nstreq_device_key=k\n' >>$INST
env PATH="$T/fw:$PATH" FAKE_UBOOT=$INST CERBERUS_DIST=file://$REL sh /src/uninstall.sh >uninst-old.log 2>&1
check "uninstall.sh still removes it" "$?$(yes_if test -e /usr/sbin/cerberus)$(yes_if test -e /etc/init.d/S99cerberus)" "0nono"
check "and every key, either name" "$(grep -c -E '^(cerberus|streq)_' $INST)" "0"
env PATH="$T/fw:$PATH" FAKE_UBOOT=$INST CERBERUS_DIST=file://$T/nowhere sh /src/uninstall.sh >uninst-nodist.log 2>&1
check "uninstall.sh without the release says E14" "$?" "14"

# ------------------------------------------------------------------ motion
section "motion events"
HOOK=$T/hooks/motion.sh
SNAP=http://root:12345@127.0.0.1:8081/image.jpg
mkdir -p $T/hooks
printf '#!/bin/sh\necho "theirs $*" >>%s/theirs.log\n' $T >$HOOK && chmod +x $HOOK
M="CERBERUS_MOTION_HOOK=$HOOK CERBERUS_PIDFILE=$T/cerberus.pid CERBERUS_SNAPSHOT_URL=$SNAP CERBERUS_MOTION_QUIET=2"

check "snapshot: majestic's frame, with its password, de-chunked" \
	"$(env $M $A snapshot 2>/dev/null | sed -n 's/^snapshot: \([0-9]*\) bytes.*/\1/p')" "10246"
check "without the password: refused, and says how to fix it" \
	"$(CERBERUS_SNAPSHOT_URL=http://127.0.0.1:8081/image.jpg $A snapshot 2>&1 | grep -c 'cerberus_snapshot_url')" "1"

cp enrolled.env agent.env
ctl frames '{"frames":2}'
( sleep 3; sh $HOOK 1 2 3; sleep 1; sh $HOOK; ) &
runfor 12 motion.log $M
check "someone else's hook is kept aside" "$(yes_if test -x $HOOK.orig)" "yes"
check "and still called, with its arguments" "$(yes_if grep -q 'theirs 1 2 3' theirs.log)" "yes"
check "the coordinator's eventKey is taken" "$(yes_if grep -q 'motion events: the coordinator takes them' motion.log)" "yes"
check "one episode for several hook calls" \
	"$(q "print(sum(1 for r in R if r['path'].endswith('/events') and r['body'].get('phase') == 'started'))")" "1"
check "signed by the device key" \
	"$(q "print(all(r['verified'] for r in R if r['path'].endswith('/events')))")" "True"
check "frames follow, as many as asked" \
	"$(q "print(sum(1 for r in R if '/frames/' in r['path']))")" "2"
check "the first taken before the cloud was asked" 	"$(q "i = next(i for i, r in enumerate(R) if r['path'].endswith('/events') and r['body'].get('phase') == 'started'); print(R[i - 1]['path'].startswith('majestic:'))")" "True"
check "each a signed JPEG" \
	"$(q "print(all(r['verified'] and r['jpeg'] and r['contentType'] == 'image/jpeg' for r in R if '/frames/' in r['path']))")" "True"
check "and the episode ends on its own" \
	"$(q "print(any(r['path'].endswith('/events') and r['body'].get('phase') == 'ended' for r in R))")" "True"

ctl frames '{"frames":0}'
B0=$(q "print(sum(1 for r in R if '/frames/' in r['path']))")
( sleep 3; env CERBERUS_PIDFILE=$T/cerberus.pid $A motion >/dev/null; ) &
runfor 8 motion2.log $M
check "cerberus motion: the hook's signal, by hand" \
	"$(q "print(sum(1 for r in R if r['path'].endswith('/events') and r['body'].get('phase') == 'started'))")" "2"
check "no frames when the cloud asks for none" "$(q "print(sum(1 for r in R if '/frames/' in r['path']))")" "$B0"
# A pid outlives its process: whatever holds it now must not get a SIGUSR1, which kills by default.
sleep 30 &
STRANGER=$!
sed "s#$T/cerberus.pid#$T/stale.pid#" $HOOK >$T/hooks/stale.sh
echo $STRANGER >$T/stale.pid
sh $T/hooks/stale.sh
sleep 1
check "a stale pid is not signalled" "$(yes_if kill -0 $STRANGER)" "yes"
kill $STRANGER 2>/dev/null

# ------------------------------------------------------------------ revocation
section "revocation and removal"
cp enrolled.env agent.env
ctl forget '{}'
F0=$(date +%s)
runfor 20 gone.log
check "a forgotten node discards its enrolment" "$(yes_if grep -q 'enrolment discarded' gone.log)" "yes"
check "and stops by itself" "$([ $(( $(date +%s) - F0 )) -lt 15 ] && echo yes || echo no)" "yes"
check "node id cleared" "$(sv cerberus_node_id agent.env)" ""
check "hub endpoint cleared" "$(sv cerberus_hub_endpoint agent.env)" ""
check "device key kept" "$(sv cerberus_device_key agent.env)" "$PRIV"
check "claim code kept" "$(sv cerberus_claim_code agent.env)" "$CLAIM"

# The migration must never undo a revocation: forgetting removes the streq_* copies too.
check "U-Boot store: a revoked node is forgotten" "$( (uboot $OLD env CERBERUS_HEARTBEAT=5 timeout 20 $A run >gone-ub.log 2>&1); yes_if grep -q 'enrolment discarded' gone-ub.log)" "yes"
check "under both names" "$(ub $OLD cerberus_node_id)$(ub $OLD streq_node_id)" ""
check "and stays forgotten on the next start" "$(uboot $OLD $A status 2>/dev/null | sed -n 's/^node  *//p')" "(not enrolled)"
check "with the device key kept" "$(ub $OLD cerberus_device_key)" "$PRIV"

mkdir -p /etc/init.d $T/victim
cp $A $T/victim/cerberus && touch /etc/init.d/S99cerberus
cp enrolled.env clean.env
CERBERUS_STORE=file:$T/clean.env CERBERUS_MOTION_HOOK=$HOOK $T/victim/cerberus clean >clean.log 2>&1
check "clean: their motion hook is back" "$(yes_if grep -q theirs $HOOK)$(yes_if test -e $HOOK.orig)" "yesno"
check "clean: removes the init script" "$(yes_if test -e /etc/init.d/S99cerberus)" "no"
check "the stored material" "$(yes_if test -e $T/clean.env)" "no"
sleep 4
check "and, a moment later, itself" "$(yes_if test -e $T/victim/cerberus)" "no"

# ------------------------------------------------------------------ installers
section "shipped scripts"
for s in install.sh uninstall.sh; do
	check "$s parses" "$(yes_if sh -n /src/$s)" "yes"
done
for s in install.sh uninstall.sh; do
	check "$s carries no comments" "$(tail -n +2 /src/$s | grep -c '^[[:space:]]*#')" "0"
done

# The binary carries the init script; no copy of it is shipped as a file.
check "init-script - prints the init script" "$($A init-script - 2>/dev/null | head -1)" "#!/bin/sh"
check "which starts the binary that wrote it" "$($A init-script - 2>/dev/null | daemon_of)" "$A"
if command -v shellcheck >/dev/null 2>&1; then
	# SC2155: `export X="$(fw_printenv ...)"` masking the exit status is the point — a key that is
	# not set is simply empty.
	$A init-script - 2>/dev/null >$T/S99lint
	check "and is shellcheck-clean" \
		"$(shellcheck -S warning -e SC2155 $T/S99lint >$T/S99lint.log 2>&1 && echo yes || { dump $T/S99lint.log >&2; echo no; })" "yes"
else
	skip "the init script under shellcheck (not installed here)"
fi

# make install has no init script to copy: it runs the binary it built to write one, pointing at
# the installed binary rather than the build tree — or refuses when that binary cannot run here.
ROOT=$T/rootfs
check "make install writes the init script" \
	"$(yes_if make -C /build install DESTDIR=$ROOT)$(yes_if test -x $ROOT/etc/init.d/S99cerberus)" "yesyes"
check "which starts the installed binary" "$(daemon_of <$ROOT/etc/init.d/S99cerberus)" "/usr/sbin/cerberus"
$A init-script - 2>/dev/null | sed "s|^DAEMON=.*|DAEMON='/usr/sbin/cerberus'|" >$T/S99want
check "and is otherwise what the binary writes" "$(yes_if cmp $T/S99want $ROOT/etc/init.d/S99cerberus)" "yes"
check "a binary that cannot run here: make install refuses" \
	"$(yes_if make -C /build install DESTDIR=$T/cross RUN=false)$(yes_if test -e $T/cross/etc/init.d/S99cerberus)" "nono"
check "unless told the firmware writes it" \
	"$(yes_if make -C /build install DESTDIR=$T/cross RUN=false INIT_SCRIPT=no)$(yes_if test -x $T/cross/usr/sbin/cerberus)" "yesyes"
$A init-script $T/initd/S99cerberus >/dev/null 2>&1
check "init-script PATH writes an executable script" "$(yes_if test -x $T/initd/S99cerberus)" "yes"
check "that parses" "$(yes_if sh -n $T/initd/S99cerberus)" "yes"
check "and leaves no temporary behind" "$(find $T/initd -name 'S99cerberus.*' | wc -l)" "0"
# A binary kept somewhere with a space and a quote in the path: one shell word, and it still starts.
ODD="$T/SD Card/it's"
mkdir -p "$ODD" && cp $A "$ODD/cerberus"
"$ODD/cerberus" init-script $T/initd/S99odd >/dev/null 2>&1
check "init-script quotes an odd path" "$(daemon_of <$T/initd/S99odd)" "$ODD/cerberus"
check "and the script runs that binary" \
	"$(env PATH="$T/fw:$PATH" FAKE_UBOOT=$T/empty.env CERBERUS_STORE=file:$T/nothing.env sh $T/initd/S99odd status 2>/dev/null | sed -n 's/^node  *//p')" \
	"(not enrolled)"

echo
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
