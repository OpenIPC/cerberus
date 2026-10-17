# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Local builds and checks

Everything runs in Docker. Nothing is installed on the host: no compiler, mbedTLS or
shellcheck. shellcheck is pinned to a release and the build image to a Debian release
(bookworm, mbedTLS 2.28). The `docker run` commands pass `--pull never`, so a check never
pulls a different version than the last run. On a fresh machine, pull the images once:

```sh
docker pull koalaman/shellcheck:v0.10.0
docker pull debian:bookworm-slim
```

The CI `lint` job checks:

```sh
# shell scripts
docker run --rm --pull never -v "$PWD:/mnt" -w /mnt koalaman/shellcheck:v0.10.0 \
  -S warning install.sh uninstall.sh ci/build.sh ci/assemble.sh tests/run.sh tests/suite.sh

# native build (mbedTLS 2.28 from Debian bookworm), then lint the init script the binary writes
docker run --rm --pull never -v "$PWD:/src:ro" debian:bookworm-slim sh -c \
  'apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc make libc6-dev libmbedtls-dev >/dev/null &&
   cp -r /src/src /tmp/s && make -s -C /tmp/s EXTRA_CFLAGS=-Werror >&2 && /tmp/s/cerberus init-script -' > /tmp/cerberus.init
docker run --rm -i --pull never koalaman/shellcheck:v0.10.0 -S warning -e SC2155 -s sh - < /tmp/cerberus.init
```

Use `koalaman/shellcheck:v0.10.0`, not `:stable`: the `stable` tag moves between releases.
`make -s` sends its size line to stderr (`>&2`); without that it lands in the script and the
lint fails with SC2275.

A pull request that changes `src/`, the installers, the `Makefile` or `ci/` also needs a new
`CERBERUS_VERSION` in `src/cerberus.c` and its section in `CHANGELOG.md`; CI checks that with
`.github/check-version.sh`, which runs the same locally against `origin/master`.

Tests and release binaries already run in containers through the repository's own scripts:

```sh
tests/run.sh                                            # Alpine (default image in tests/run.sh), musl + mbedTLS 3.x
CERBERUS_TEST_IMAGE=debian:bookworm-slim tests/run.sh   # glibc + mbedTLS 2.28
ci/build.sh arm                                         # static musl binary; all four without arguments
```

## Rules

- Never build on the host. `make -C src` outside a container leaves a host-built `src/cerberus`
  behind.
- Local builds are for development only. Release binaries come from CI, never from a local build.
