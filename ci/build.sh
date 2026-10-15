#!/usr/bin/env bash
# Builds the release binaries: one static musl binary per CPU architecture.
#
# Static musl is the whole point. A camera's firmware may be musl or uClibc or glibc, may carry
# mbedTLS 2.28, 3.x or none at all — a static binary does not care, so one build per architecture
# runs on every camera of that architecture, whatever SoC and firmware it has:
#
#   arm      ARMv5TE, soft-float EABI — runs on every ARM camera from ARMv5 (no VFP) to ARMv7
#            (HiSilicon, Goke, SigmaStar, Fullhan, ...). Floating point never matters here: the
#            crypto is integer, and soft-float code runs on a CPU with an FPU just as well.
#   mipsel   MIPS32 little-endian — Ingenic T10/T20/T21/T31/T40 and other MIPS cameras.
#   aarch64  64-bit ARM.
#   x86_64   VMs and test boxes.
#
# Everything happens in a Debian container, so the host needs only Docker: the Bootlin musl
# toolchain and the mbedTLS source are downloaded once into .cache/ and checked against pinned
# sha256 sums. Each binary is then *run* under QEMU in the same container — `--help`, and a keypair
# compared with what `wg pubkey` derives — because a binary for the right ELF machine can still die
# on the first instruction its CPU does not have, and that is invisible to `file`.
#
# Usage: ci/build.sh                          # all four
#        ci/build.sh arm mipsel               # a subset
#        VERSION=1.2.0 OUT=dist ci/build.sh
#
# VERSION defaults to the nearest v* tag, without the v. It is compiled into the binary — it is what
# the agent reports to the coordinator. With no tag in reach the binary keeps the version in src/cerberus.c.
#
# Output: $OUT/cerberus-<arch>. Names carry no version on purpose: the installer fetches
# releases/latest/download/cerberus-<arch>, and that address must not change between releases.
set -euo pipefail

cd "$(dirname "$0")/.."

ARCHES=("$@")
[ ${#ARCHES[@]} -gt 0 ] || ARCHES=(arm mipsel aarch64 x86_64)
OUT=${OUT:-dist}
CACHE=${CACHE:-.cache}
IMAGE=${CERBERUS_BUILD_IMAGE:-debian:bookworm-slim}

TOOLCHAIN_RELEASE=stable-2025.08-1
MBEDTLS_VERSION=3.6.7
MBEDTLS_SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6

VERSION=${VERSION:-$(git describe --tags --match 'v[0-9]*' --dirty 2>/dev/null || true)}
VERSION=${VERSION#v}

mkdir -p "$OUT" "$CACHE"

# Docker on Windows wants a native path for -v, and MSYS would rewrite the container-side one.
host_path() {
	case "$(uname -s)" in
		MINGW*|MSYS*|CYGWIN*) (cd "$1" && pwd -W) ;;
		*) (cd "$1" && pwd) ;;
	esac
}

for arch in "${ARCHES[@]}"; do
	case "$arch" in
		arm)
			tc=armv5-eabi; tc_sha=8cdb4ad70c6b5a66427fa3315fe3ddde1c19c90232674dec59045e04d2a36cf1
			qemu="qemu-arm-static"; want='ARM, EABI5' ;;
		mipsel)
			tc=mips32el; tc_sha=12a5e2f50b94b5c619730914db380ce9679798fb3c7460a40d480f77bb50628f
			qemu="qemu-mipsel-static"; want='LSB executable, MIPS, MIPS32' ;;
		aarch64)
			tc=aarch64; tc_sha=defba831ffa1175236f137069333e21ed46d4d19feb5080a90cf248b6fc2cb08
			qemu="qemu-aarch64-static"; want='ARM aarch64' ;;
		x86_64)
			tc=x86-64; tc_sha=09fca3aa89540f1b01b5f4210d488cbeb00f522044c53e9989b1dd8a38076912
			qemu=; want='x86-64' ;;
		*) echo "unknown arch '$arch' (want arm | mipsel | aarch64 | x86_64)"; exit 2 ;;
	esac

	echo "==> cerberus-$arch (Bootlin $tc musl $TOOLCHAIN_RELEASE, mbedTLS $MBEDTLS_VERSION, version ${VERSION:-from cerberus.c})"

	# -i matters: without it the container gets no stdin, `sh -s` reads EOF, and the build silently
	# does nothing while exiting 0.
	MSYS_NO_PATHCONV=1 docker run -i --rm \
		-v "$(host_path .):/src:ro" \
		-v "$(host_path "$CACHE"):/cache" \
		-v "$(host_path "$OUT"):/out" \
		-e ARCH="$arch" -e TC="$tc" -e TC_SHA="$tc_sha" -e TC_RELEASE="$TOOLCHAIN_RELEASE" \
		-e MBEDTLS_VERSION="$MBEDTLS_VERSION" -e MBEDTLS_SHA256="$MBEDTLS_SHA256" \
		-e QEMU="$qemu" -e WANT="$want" -e VERSION="$VERSION" \
		"$IMAGE" bash -s <<'BUILD'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends ca-certificates curl xz-utils bzip2 make file \
	qemu-user-static wireguard-tools openssl >/dev/null

fetch() { # fetch <url> <sha256> <file in /cache>
	if [ ! -f "/cache/$3" ] || ! echo "$2  /cache/$3" | sha256sum -c --quiet - 2>/dev/null; then
		curl -fsSL --retry 3 -o "/cache/$3.part" "$1"
		echo "$2  /cache/$3.part" | sha256sum -c --quiet -
		mv "/cache/$3.part" "/cache/$3"
	fi
}

tarball="$TC--musl--$TC_RELEASE.tar.xz"
fetch "https://toolchains.bootlin.com/downloads/releases/toolchains/$TC/tarballs/$tarball" "$TC_SHA" "$tarball"
fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2" \
	"$MBEDTLS_SHA256" "mbedtls-$MBEDTLS_VERSION.tar.bz2"

mkdir -p /opt/tc && tar -xJf "/cache/$tarball" -C /opt/tc --strip-components=1
CC=$(ls /opt/tc/bin/*-buildroot-linux-*-gcc | head -1)
CROSS=${CC%gcc}

# mbedTLS as static libraries for this toolchain: whatever a firmware carries never enters into it.
mkdir -p /build/mbedtls /build/prefix/lib
tar -xjf "/cache/mbedtls-$MBEDTLS_VERSION.tar.bz2" -C /build/mbedtls --strip-components=1
# PSA takes its randomness from cerberus's own generator instead of gathering it from the kernel on
# every start: on some cameras that read blocks for minutes ("Randomness" in src/cerberus.c).
cfg=/build/mbedtls/include/mbedtls/mbedtls_config.h
sed -i 's|^//#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG$|#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG|' "$cfg"
grep -q '^#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG$' "$cfg"

# Everything the agent never uses, out: with the default configuration mbedTLS was most of the binary,
# kept alive by the cipher-suite tables whether a suite could ever be picked or not. What stays is
# what a TLS 1.2/1.3 *client* needs against a public CA — the coordinator is Let's Encrypt (ECDSA
# P-256 leaf, P-384 intermediates, an RSA root behind the X1 cross-sign), other CAs are RSA or
# ECDSA too: ECDHE-ECDSA/ECDHE-RSA, AES-GCM, SHA-256/384 (and SHA-1/512 for older chains), P-256,
# P-384 and X25519 (the device key and a TLS group), RSA and ECDSA verification, PEM and X.509
# parsing. A name below that this mbedTLS does not know fails the build: a rename upstream must not
# quietly put something back.
for o in \
	MBEDTLS_SSL_SRV_C MBEDTLS_SSL_PROTO_DTLS MBEDTLS_SSL_DTLS_ANTI_REPLAY MBEDTLS_SSL_DTLS_HELLO_VERIFY \
	MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE MBEDTLS_SSL_DTLS_CONNECTION_ID MBEDTLS_SSL_DTLS_SRTP \
	MBEDTLS_SSL_COOKIE_C MBEDTLS_SSL_CACHE_C MBEDTLS_SSL_TICKET_C MBEDTLS_SSL_SESSION_TICKETS \
	MBEDTLS_SSL_EARLY_DATA MBEDTLS_SSL_RENEGOTIATION MBEDTLS_SSL_CONTEXT_SERIALIZATION MBEDTLS_SSL_ALPN \
	MBEDTLS_KEY_EXCHANGE_PSK_ENABLED MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED \
	MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED MBEDTLS_KEY_EXCHANGE_RSA_ENABLED MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED \
	MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED \
	MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED \
	MBEDTLS_DES_C MBEDTLS_CAMELLIA_C MBEDTLS_ARIA_C MBEDTLS_CCM_C MBEDTLS_CMAC_C MBEDTLS_NIST_KW_C \
	MBEDTLS_CHACHAPOLY_C MBEDTLS_CHACHA20_C MBEDTLS_POLY1305_C \
	MBEDTLS_CIPHER_MODE_CBC MBEDTLS_CIPHER_MODE_CFB MBEDTLS_CIPHER_MODE_CTR MBEDTLS_CIPHER_MODE_OFB \
	MBEDTLS_CIPHER_MODE_XTS MBEDTLS_CIPHER_PADDING_PKCS7 MBEDTLS_CIPHER_PADDING_ONE_AND_ZEROS \
	MBEDTLS_CIPHER_PADDING_ZEROS_AND_LEN MBEDTLS_CIPHER_PADDING_ZEROS \
	MBEDTLS_MD5_C MBEDTLS_RIPEMD160_C MBEDTLS_SHA3_C MBEDTLS_DHM_C MBEDTLS_ECJPAKE_C MBEDTLS_LMS_C \
	MBEDTLS_HMAC_DRBG_C MBEDTLS_ECDSA_DETERMINISTIC MBEDTLS_GENPRIME \
	MBEDTLS_ECP_DP_SECP192R1_ENABLED MBEDTLS_ECP_DP_SECP224R1_ENABLED MBEDTLS_ECP_DP_SECP521R1_ENABLED \
	MBEDTLS_ECP_DP_SECP192K1_ENABLED MBEDTLS_ECP_DP_SECP224K1_ENABLED MBEDTLS_ECP_DP_SECP256K1_ENABLED \
	MBEDTLS_ECP_DP_BP256R1_ENABLED MBEDTLS_ECP_DP_BP384R1_ENABLED MBEDTLS_ECP_DP_BP512R1_ENABLED \
	MBEDTLS_ECP_DP_CURVE448_ENABLED MBEDTLS_PK_PARSE_EC_EXTENDED MBEDTLS_PK_PARSE_EC_COMPRESSED \
	MBEDTLS_PKCS5_C MBEDTLS_PKCS7_C MBEDTLS_PKCS12_C MBEDTLS_PK_WRITE_C MBEDTLS_PEM_WRITE_C \
	MBEDTLS_X509_CREATE_C MBEDTLS_X509_CRT_WRITE_C MBEDTLS_X509_CSR_WRITE_C MBEDTLS_X509_CSR_PARSE_C \
	MBEDTLS_X509_CRL_PARSE_C \
	MBEDTLS_PSA_CRYPTO_STORAGE_C MBEDTLS_PSA_ITS_FILE_C \
	MBEDTLS_ERROR_C MBEDTLS_VERSION_FEATURES MBEDTLS_SELF_TEST MBEDTLS_DEBUG_C MBEDTLS_TIMING_C
do
	grep -q "define $o\$" "$cfg" || { echo "mbedTLS $MBEDTLS_VERSION has no $o"; exit 1; }
	sed -i "s|^#define $o\$|//#define $o|" "$cfg"
done

make -s -C /build/mbedtls/library static CC="$CC" AR="${CROSS}ar" \
	CFLAGS="-Os -ffunction-sections -fdata-sections" >/dev/null
cp /build/mbedtls/library/*.a /build/prefix/lib/
cp -r /build/mbedtls/include /build/prefix/

cp /src/src/cerberus.c /src/src/Makefile /build/
cd /build
make CC="$CC" STRIP="${CROSS}strip" MBEDTLS=/build/prefix STATIC=1 VERSION="$VERSION" EXTRA_CFLAGS=-Werror

# The wrong ELF machine, or a dynamic binary, is the failure worth catching here — and it is
# invisible in a build log.
file cerberus
file cerberus | grep -q "$WANT"
file cerberus | grep -q "statically linked"

# ...and so is a binary that is the right machine and still cannot run. Run it.
run() { if [ -n "$QEMU" ]; then "$QEMU" /build/cerberus "$@"; else /build/cerberus "$@"; fi; }
run --help 2>&1 | head -1
CERBERUS_STORE=file:/tmp/smoke.env run keygen >/tmp/keys 2>/dev/null
[ "$(sed -n 1p /tmp/keys | wg pubkey)" = "$(sed -n 2p /tmp/keys)" ] \
	|| { echo "the X25519 key this binary derives is not the one WireGuard derives"; exit 1; }
echo "keygen on $ARCH matches wg pubkey"

# ...and TLS, against what the configuration above has to keep: TLS 1.3 and 1.2, ECDSA and RSA
# servers (RSA under TLS 1.3 signs with RSA-PSS — the option that is easiest to trim by mistake),
# a chain from an RSA root through an ECDSA intermediate as Let's Encrypt's is, and two servers that
# must be refused: one from another CA, one with a trusted certificate for another name. Local
# servers only: a release build does not depend on the net.
cd /tmp
ossl() { openssl "$@" >/dev/null 2>&1; }
ossl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 -subj /CN=ca -keyout ca.key -out ca.crt
ossl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 -subj /CN=rogue -keyout rogue.key -out rogue.crt
ossl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=rsa-root -keyout rroot.key -out rroot.crt
printf 'subjectAltName=DNS:localhost\n' >leaf.ext
printf 'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign\n' >inter.ext
leaf() { # leaf <name> <issuer> <ext> <newkey args...>
	n=$1 i=$2 e=$3; shift 3
	ossl req -newkey "$@" -nodes -subj "/CN=$n" -keyout "$n.key" -out "$n.csr"
	ossl x509 -req -in "$n.csr" -CA "$i.crt" -CAkey "$i.key" -CAcreateserial -days 1 -extfile "$e" -out "$n.crt"
}
printf 'subjectAltName=DNS:elsewhere.test\n' >other.ext
leaf ec ca leaf.ext ec -pkeyopt ec_paramgen_curve:prime256v1
leaf other ca other.ext ec -pkeyopt ec_paramgen_curve:prime256v1
leaf rsa ca leaf.ext rsa:2048
leaf inter rroot inter.ext ec -pkeyopt ec_paramgen_curve:secp384r1
leaf chain inter leaf.ext ec -pkeyopt ec_paramgen_curve:prime256v1
port=8440
tls() { # tls <label> <want ok|refused> <ca> <cert> <s_server args...>
	label=$1 want=$2 ca=$3 cert=$4; shift 4
	port=$((port + 1))
	openssl s_server -quiet -www -accept $port -cert "$cert.crt" -key "$cert.key" "$@" >/dev/null 2>&1 &
	srv=$!
	# Listening, or the case proves nothing: a refusal must come from the certificate, not a closed port.
	for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
	(exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null || { echo "TLS on $ARCH: $label — the test server did not start"; exit 1; }
	out=$(CERBERUS_CA=/tmp/$ca.crt CERBERUS_SNAPSHOT_URL=https://localhost:$port/ run snapshot 2>&1) && rc=0 || rc=$?
	kill $srv 2>/dev/null || true
	# `snapshot` exits 1 even on success here: s_server's page is not a JPEG. What decides is what it
	# saw — the page over TLS, or a handshake refused by certificate verification (-0x2700). A crash,
	# a timeout or any other error is neither, and fails the build.
	case "$out" in
		*"TLS ok"*"HTTP 200"*) got=ok ;;
		*"handshake with localhost failed (-0x2700)"*) got=refused ;;
		*) got="something else (exit $rc)" ;;
	esac
	[ "$got" = "$want" ] || { echo "TLS on $ARCH: $label — $got, want $want"; echo "$out"; exit 1; }
	echo "TLS on $ARCH: $label — $got"
}
tls "1.3, ECDSA P-256"                 ok      ca    ec    -tls1_3
tls "1.3, RSA (RSA-PSS)"               ok      ca    rsa   -tls1_3
tls "1.2, ECDHE-ECDSA"                 ok      ca    ec    -tls1_2
tls "1.2, ECDHE-RSA"                   ok      ca    rsa   -tls1_2
tls "RSA root, ECDSA intermediate"     ok      rroot chain -cert_chain inter.crt
tls "a certificate from another CA"    refused rogue ec
tls "a certificate for another name"   refused ca    other
cd /build

cp cerberus "/out/cerberus-$ARCH"
BUILD

	echo "    $OUT/cerberus-$arch  ($(wc -c <"$OUT/cerberus-$arch") bytes)"
done
