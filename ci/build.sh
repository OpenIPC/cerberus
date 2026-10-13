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
# the agent reports to the coordinator. With no tag in reach the binary keeps the version in main.c.
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

	echo "==> cerberus-$arch (Bootlin $tc musl $TOOLCHAIN_RELEASE, mbedTLS $MBEDTLS_VERSION, version ${VERSION:-from main.c})"

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
	qemu-user-static wireguard-tools >/dev/null

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
make -s -C /build/mbedtls/library static CC="$CC" AR="${CROSS}ar" \
	CFLAGS="-Os -ffunction-sections -fdata-sections" >/dev/null
cp /build/mbedtls/library/*.a /build/prefix/lib/
cp -r /build/mbedtls/include /build/prefix/

cp /src/main.c /src/Makefile /build/
cd /build
make CC="$CC" STRIP="${CROSS}strip" MBEDTLS=/build/prefix STATIC=1 VERSION="$VERSION" EXTRA_CFLAGS=-Werror

# The wrong ELF machine, or a dynamic binary, is the failure worth catching here — and it is
# invisible in a build log.
file cerberus
file cerberus | grep -q "$WANT"
file cerberus | grep -q "statically linked"

# ...and so is a binary that is the right machine and still cannot run. Run it.
run() { if [ -n "$QEMU" ]; then "$QEMU" ./cerberus "$@"; else ./cerberus "$@"; fi; }
run --help 2>&1 | head -1
CERBERUS_STORE=file:/tmp/smoke.env run keygen >/tmp/keys 2>/dev/null
[ "$(sed -n 1p /tmp/keys | wg pubkey)" = "$(sed -n 2p /tmp/keys)" ] \
	|| { echo "the X25519 key this binary derives is not the one WireGuard derives"; exit 1; }
echo "keygen on $ARCH matches wg pubkey"

cp cerberus "/out/cerberus-$ARCH"
BUILD

	echo "    $OUT/cerberus-$arch  ($(wc -c <"$OUT/cerberus-$arch") bytes)"
done
