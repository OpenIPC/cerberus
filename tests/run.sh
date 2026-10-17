#!/usr/bin/env bash
# Runs tests/suite.sh in a throwaway container: no C toolchain, mbedTLS or WireGuard tools needed on
# the host — only Docker. The container is --privileged because the data-plane section creates a
# WireGuard interface and a network namespace for the hub; the host kernel needs WireGuard for that
# part (any current Linux, and Docker Desktop's VM), and the section is skipped where it is missing.
#
# Usage: tests/run.sh                                  # Alpine: musl + mbedTLS 3.x (what OpenIPC is)
#        CERBERUS_TEST_IMAGE=debian:bookworm-slim tests/run.sh   # glibc + mbedTLS 2.28 LTS
#        REQUIRE_WG=1 tests/run.sh                     # a missing data plane is a failure (CI)
set -euo pipefail

cd "$(dirname "$0")/.."
IMAGE=${CERBERUS_TEST_IMAGE:-alpine:3.22}

host_path() {
	case "$(uname -s)" in
		MINGW*|MSYS*|CYGWIN*) (cd "$1" && pwd -W) ;;
		*) (cd "$1" && pwd) ;;
	esac
}

echo "==> cerberus tests in $IMAGE"
MSYS_NO_PATHCONV=1 exec docker run --rm --privileged \
	--add-host coordinator.test:127.0.0.1 \
	-v "$(host_path .):/src:ro" \
	-e REQUIRE_WG="${REQUIRE_WG:-}" \
	"$IMAGE" sh /src/tests/suite.sh
