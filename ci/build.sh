#!/usr/bin/env bash
# Package the on-camera agent for real hardware.
#
# The camera is not an edge box: no room for a runtime, and the rootfs is a read-only squashfs. So the
# agent is one C binary, mbedTLS its only dependency, using the kernel WireGuard the firmware ships.
#
# Cross-compiles inside a Debian container with multiarch, so the only thing a build host needs is
# Docker — no toolchain, no emulation, no arm hardware.
#
# Usage: ci/build.sh                       # arm64
#        ci/build.sh arm64 armhf amd64     # explicit set (Debian arch names)
#        ci/build.sh musl-armv7            # OpenIPC (ARM): static musl, no dependencies
#        VERSION=1.2.0 OUT=dist ci/build.sh
#
# VERSION defaults to the nearest v* tag, without the v. It is compiled into the binary — it is what
# the agent reports to the coordinator — so a tarball and the agent inside it never disagree. With no
# tag in reach the binary keeps the version written in main.c and the tarball is named <that>-dev.
#
# NOTE ON LIBC. arm64/armhf/amd64 here are glibc, which is what a generic arm SBC runs, and they are
# dynamically linked on purpose: statically linked glibc still needs matching shared NSS libraries at
# runtime for getaddrinfo — exactly the call that resolves the hub endpoint. They are also the
# mbedTLS 2.28 builds (Debian bookworm), the generation Buildroot-era firmware carries.
#
# OpenIPC is musl, so a glibc binary does not run there at all: the loader it names does not exist and
# the kernel answers with a bare "not found". That is what `musl-armv7` is for. It builds inside an
# armv7 Alpine container (binfmt/QEMU — Docker Desktop has it; on a plain Linux host run
# `docker run --privileged --rm tonistiigi/binfmt --install arm` once) and links statically, because
# static is the only thing that makes a binary independent of which mbedTLS a given firmware carries.
# musl static has none of glibc's NSS problem.
#
# It does not cover every OpenIPC device: Ingenic T20/T31 are MIPS, and ARMv5 SoCs have no VFP, so
# neither runs an ARMv7 build. Those need the firmware's own Buildroot toolchain:
#
#     make CC=arm-openipc-linux-musleabi-gcc MBEDTLS=<buildroot-staging> STATIC=1
set -euo pipefail

cd "$(dirname "$0")/.."

ARCHES=("$@")
[ ${#ARCHES[@]} -gt 0 ] || ARCHES=(arm64)
OUT=${OUT:-dist}
IMAGE=${CERBERUS_BUILD_IMAGE:-debian:bookworm-slim}
# --platform picks the architecture; the single-architecture repository keeps that choice from leaking.
# `--platform linux/arm/v7 alpine:3.20` overwrites the local alpine:3.20 tag with an armv7 image, and
# every later `docker run alpine:3.20` on that machine then runs emulated — tests/run.sh included,
# where kernel WireGuard fails and it reads as a bug in the agent.
CERBERUS_ARM_IMAGE=${CERBERUS_ARM_IMAGE:-arm32v7/alpine:3.20}

VERSION=${VERSION:-$(git describe --tags --match 'v[0-9]*' --dirty 2>/dev/null || true)}
VERSION=${VERSION#v}
SRC_VERSION=$(sed -n 's/^#define CERBERUS_VERSION "\(.*\)"$/\1/p' main.c)
NAME_VERSION=${VERSION:-$SRC_VERSION-dev}

mkdir -p "$OUT"

# Docker on Windows wants a native path for -v, and MSYS would rewrite the container-side one.
host_path() {
	case "$(uname -s)" in
		MINGW*|MSYS*|CYGWIN*) (cd "$1" && pwd -W) ;;
		*) (cd "$1" && pwd) ;;
	esac
}
SRC=$(host_path .)

for arch in "${ARCHES[@]}"; do
	case "$arch" in
		arm64) triple=aarch64-linux-gnu; want='ARM aarch64' ;;
		armhf) triple=arm-linux-gnueabihf; want='ARM, EABI' ;;
		amd64) triple=x86_64-linux-gnu; want='x86-64' ;;
		musl-armv7) triple=armv7-alpine-linux-musleabihf; want='ARM, EABI' ;;
		*) echo "unknown arch '$arch' (want arm64 | armhf | amd64 | musl-armv7)"; exit 2 ;;
	esac

	echo "==> building linux-$arch ($triple), version ${VERSION:-$SRC_VERSION (from main.c)}"
	stage="$OUT/cerberus-$NAME_VERSION-linux-$arch"
	rm -rf "$stage"
	mkdir -p "$stage"

	if [ "$arch" = musl-armv7 ]; then
		# Native build inside an emulated armv7 Alpine: no cross toolchain to find, and the libc is
		# the one OpenIPC actually runs. Slow (QEMU) but this is one C file.
		MSYS_NO_PATHCONV=1 docker run -i --rm \
			--platform linux/arm/v7 \
			-v "$SRC:/src:ro" \
			-v "$(host_path "$stage"):/out" \
			-e WANT="$want" -e VERSION="$VERSION" \
			"$CERBERUS_ARM_IMAGE" sh -s <<'MUSLBUILD' || {
set -e
apk add --no-cache gcc musl-dev make mbedtls-dev mbedtls-static file >/dev/null
mkdir /build && cp /src/main.c /src/Makefile /build/ && cd /build
make STATIC=1 VERSION="$VERSION" EXTRA_CFLAGS=-Werror

file cerberus
file cerberus | grep -q "$WANT"
# Static is the point: a firmware that carries no mbedTLS, or a different one, must not matter.
file cerberus | grep -q "statically linked"

cp cerberus /out/
MUSLBUILD
			echo "musl-armv7 build failed. If docker could not run linux/arm/v7, install binfmt:"
			echo "  docker run --privileged --rm tonistiigi/binfmt --install arm"
			exit 1
		}
	else

	# -i matters: without it the container gets no stdin, `sh -s` reads EOF, and the build silently
	# does nothing while exiting 0.
	MSYS_NO_PATHCONV=1 docker run -i --rm \
		-v "$SRC:/src:ro" \
		-v "$(host_path "$stage"):/out" \
		-e ARCH="$arch" -e TRIPLE="$triple" -e WANT="$want" -e VERSION="$VERSION" \
		"$IMAGE" sh -s <<'BUILD'
set -e
export DEBIAN_FRONTEND=noninteractive

# amd64 is the container's own architecture: no multiarch, no cross toolchain, plain gcc.
if [ "$ARCH" = amd64 ]; then
	apt-get update -qq >/dev/null
	apt-get install -y -qq gcc libc6-dev libmbedtls-dev make file >/dev/null
	CC=gcc
	STRIP=strip
else
	dpkg --add-architecture "$ARCH"
	apt-get update -qq >/dev/null
	apt-get install -y -qq "crossbuild-essential-$ARCH" "libmbedtls-dev:$ARCH" make file >/dev/null
	CC="$TRIPLE-gcc"
	STRIP="$TRIPLE-strip"
fi

mkdir /build && cp /src/main.c /src/Makefile /build/ && cd /build
make CC="$CC" STRIP="$STRIP" VERSION="$VERSION" EXTRA_CFLAGS=-Werror

# A binary built for the wrong architecture is the failure mode worth catching here, and it is
# invisible in a build log.
file cerberus
file cerberus | grep -q "$WANT"

# Its one shared dependency has to actually be mbedTLS and nothing more exotic.
"${TRIPLE}-objdump" -p cerberus 2>/dev/null | grep NEEDED || objdump -p cerberus | grep NEEDED

cp cerberus /out/
BUILD
	fi

	cp S99cerberus "$stage/S99cerberus"
	chmod 755 "$stage/S99cerberus"
	# The licence travels with every copy, binaries included: its Notices rule asks for exactly that.
	cp LICENSE.md "$stage/LICENSE.md"

	cat >"$stage/install.sh" <<'EOF'
#!/bin/sh
set -e
[ "$(id -u)" = 0 ] || { echo "cerberus: E10" >&2; exit 10; }

cp cerberus /usr/sbin/cerberus
cp S99cerberus /etc/init.d/S99cerberus
chmod 0755 /usr/sbin/cerberus /etc/init.d/S99cerberus
mkdir -p /etc/streq && chmod 0700 /etc/streq

echo "done. Provisioning is two fw_setenv calls and a reboot:"
echo "  fw_setenv streq_api https://api.example.com/v1"
echo "  fw_setenv streq_enrollment_token tok_..."
echo "  reboot"
echo
echo "After start the claim code is shown by: cerberus status"
EOF
	chmod 755 "$stage/install.sh"

	if [ "$arch" = musl-armv7 ]; then
		LIBC_NOTE="**Static musl build for ARMv7 — for OpenIPC firmware.** No dependencies at all:
mbedTLS is inside the binary, so whichever mbedTLS the firmware carries does not matter. It does not
fit MIPS devices (Ingenic T20/T31) or ARMv5 without VFP — those need the firmware's own toolchain."
	else
		LIBC_NOTE='**This is a glibc build for a generic arm SBC.** It will not start on OpenIPC
firmware at all — that is musl: a camera needs the `musl-armv7` build.'
	fi

	cat >"$stage/README.md" <<EOF
# cerberus ($NAME_VERSION, linux-$arch)

The on-camera agent: enrolls into the coordinator over TLS, keeps its join material in the U-Boot
environment via \`fw_setenv\`, brings up kernel WireGuard and supervises the tunnel. The camera needs
\`wg\`, \`ip\` and \`fw_setenv\`.

\`\`\`sh
./install.sh
fw_setenv streq_api https://api.example.com/v1
fw_setenv streq_enrollment_token tok_...
reboot
\`\`\`

$LIBC_NOTE

Full guide, variables and troubleshooting: https://cloud.openipc.ru/docs/openipc-camera/
Source: https://github.com/OpenIPC/cerberus

Licensed under the Prosperity Public License 3.0.0 — see LICENSE.md: free for noncommercial use,
a thirty-day trial for commercial use.
EOF

	# Packed inside a container, from a copy that is not on a bind mount: a Windows host has no
	# execute bit to give, and a tarball whose binary unpacks as 0644 fails at `./cerberus` — the
	# first thing anyone types after unpacking.
	MSYS_NO_PATHCONV=1 docker run -i --rm \
		-v "$(host_path "$OUT"):/out" \
		-e STAGE="$(basename "$stage")" \
		"$IMAGE" sh -s <<'PACK'
set -e
cd /tmp
cp -r "/out/$STAGE" .
cd "$STAGE"
chmod 0755 cerberus install.sh S99cerberus
chmod 0644 README.md LICENSE.md
cd /tmp
tar -czf "/out/$STAGE.tar.gz" "$STAGE"
PACK
	rm -rf "$stage"
	(cd "$OUT" && sha256sum "cerberus-$NAME_VERSION-linux-$arch.tar.gz" \
		>"cerberus-$NAME_VERSION-linux-$arch.tar.gz.sha256")
	echo "    $OUT/cerberus-$NAME_VERSION-linux-$arch.tar.gz  ($(du -h "$OUT/cerberus-$NAME_VERSION-linux-$arch.tar.gz" | cut -f1))"
done
