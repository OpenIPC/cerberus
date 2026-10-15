#!/usr/bin/env bash
# Lays a release out next to the binaries ci/build.sh left in $OUT: the installer (which also uninstalls),
# the uninstaller kept as its old address, the init script, the license, and SHA256SUMS over everything
# a camera downloads. The installer reads the sums from the same release, so a binary and the sum it
# is checked against always travel together.
#
# Usage: ci/assemble.sh            (OUT=dist by default)
set -euo pipefail

cd "$(dirname "$0")/.."
OUT=${OUT:-dist}

ls "$OUT"/cerberus-* >/dev/null 2>&1 || { echo "no $OUT/cerberus-* — run ci/build.sh first"; exit 1; }

cp install.sh uninstall.sh src/S99cerberus LICENSE.md "$OUT/"
chmod 0755 "$OUT"/cerberus-* "$OUT/install.sh" "$OUT/uninstall.sh" "$OUT/S99cerberus"

(
	cd "$OUT"
	# Plain names, no "*" binary marker: the installer matches on the bare name.
	for f in cerberus-* install.sh uninstall.sh S99cerberus; do
		printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"
	done >SHA256SUMS
	cat SHA256SUMS
)
