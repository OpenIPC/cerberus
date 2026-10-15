#!/bin/sh
set -eu

DIST="${CERBERUS_DIST:-${STREQ_DIST:-https://github.com/OpenIPC/cerberus/releases/latest/download}}"
DOCS="${CERBERUS_DOCS:-${STREQ_DOCS:-https://cloud.openipc.ru/docs/openipc-camera/#codes}}"

script=$(curl -fsL "$DIST/install.sh") || { echo "cerberus: E14" >&2; echo "cerberus: $DOCS" >&2; exit 14; }
exec sh -c "$script" install.sh --uninstall
