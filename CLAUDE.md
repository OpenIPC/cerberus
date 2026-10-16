# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Local checks run in Docker

Nothing is installed on the host: no compiler, mbedTLS or shellcheck. Every image tag below is
pinned, and each one runs with `--pull never`, so a check never pulls a different version than
the last run.

The same checks as the CI `lint` job:

```sh
# shell scripts
docker run --rm --pull never -v "$PWD:/mnt" -w /mnt koalaman/shellcheck:v0.10.0 \
  -S warning install.sh uninstall.sh ci/build.sh ci/assemble.sh tests/run.sh tests/suite.sh

# native build (mbedTLS 2.28 from Debian bookworm), then lint the init script the binary writes
docker run --rm --pull never -u "$(id -u):$(id -g)" -v "$PWD:/src:ro" compot/cross:2026.10 \
  sh -c 'cp -r /src/src /tmp/s && make -s -C /tmp/s EXTRA_CFLAGS=-Werror >&2 && /tmp/s/cerberus init-script -' \
  > /tmp/cerberus.init
docker run --rm -i --pull never koalaman/shellcheck:v0.10.0 -S warning -e SC2155 -s sh - < /tmp/cerberus.init
```

Use `koalaman/shellcheck:v0.10.0`, not `:stable`: the `stable` tag moves between releases.
`make -s` sends its size line to stderr (`>&2`); without that it lands in the script and the
lint fails with SC2275.

Tests and release binaries already run in containers through the repository's own scripts:

```sh
tests/run.sh                                            # alpine:3.20, musl + mbedTLS 3.x
CERBERUS_TEST_IMAGE=debian:bookworm-slim tests/run.sh   # glibc + mbedTLS 2.28
ci/build.sh arm                                         # static musl binary; all four without arguments
```

## Rules

- Never build on the host. `make -C src` outside a container leaves a host-built `src/cerberus`
  behind.
- Local builds are for development only. Releases come from CI on a `v*` tag.
- PRs are prepared here, then handed to @keyldev for review. They are not merged from here,
  and nothing is pushed to `master`.
