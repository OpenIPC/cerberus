![OpenIPC logo][logo]

# OpenIPC Cerberus

**English** | [Русский](README.ru.md) | [简体中文](README.zh-CN.md)

[![CI](https://github.com/OpenIPC/cerberus/actions/workflows/ci.yml/badge.svg)](https://github.com/OpenIPC/cerberus/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/OpenIPC/cerberus)](https://github.com/OpenIPC/cerberus/releases/latest)
[![License](https://img.shields.io/badge/license-Prosperity%203.0-blue)](LICENSE.md)
[![Platform](https://img.shields.io/badge/platform-OpenIPC-brightgreen)](https://openipc.org)

The cloud agent for cameras running [OpenIPC](https://openipc.org) firmware: a small C daemon that
lives **inside the camera** and puts it on a private WireGuard network, so the camera can be reached
from anywhere — video, ONVIF, settings and SSH — with nothing between it and the cloud but the
tunnel.

It does five things and nothing else:

1. generates the device's X25519 keypair and a claim code — once, on first start;
2. enrols at the coordinator over TLS (`POST /enroll`) and takes the join material;
3. keeps key, claim code and join material in the **U-Boot environment** via `fw_setenv` — the one
   place on a camera that survives a firmware reflash;
4. brings up `wg0` and supervises the tunnel from both ends: the handshake age from `wg` **and** an
   ICMP probe of the hub *inside* the tunnel — a fresh handshake over a dead path looks alive;
5. reports liveness with `POST /nodes/{id}/heartbeat` and applies what comes back: changed join
   material and one-shot commands.

**mbedTLS** is the only dependency; everything else is libc. It drives the kernel WireGuard the
firmware already ships, so it is a few hundred kilobytes static, not a userspace WireGuard.

## Install

One line over SSH on the camera, with an enrolment token from the cloud panel (the panel gives you
this command ready-filled):

```sh
curl -fsSL https://github.com/OpenIPC/cerberus/releases/latest/download/install.sh | sh -s -- tok_...
```

The installer picks the build for the camera's CPU, checks it against `SHA256SUMS` from the same
release, installs `/usr/sbin/cerberus`, has it write `/etc/init.d/S99cerberus`, puts the address and
the token into the U-Boot environment and starts the agent. No reboot is needed. On failure it prints
a short code — `cerberus: E13` — and the page that decodes it:
<https://cloud.openipc.ru/docs/openipc-camera/#codes>.

To take it all off again:

```sh
curl -fsSL https://github.com/OpenIPC/cerberus/releases/latest/download/install.sh | sh -s -- --uninstall
```

`uninstall.sh` from the same release still works and does exactly this — it is kept for commands
printed before the two scripts became one.

By hand, the same thing:

```sh
# cerberus-arm, cerberus-mipsel, ... — see Releases below
curl -fsSL -o /usr/sbin/cerberus https://github.com/OpenIPC/cerberus/releases/latest/download/cerberus-arm
chmod +x /usr/sbin/cerberus
cerberus init-script                   # writes /etc/init.d/S99cerberus, which starts this binary
fw_setenv cerberus_api https://cloud-api.openipc.ru/v1
fw_setenv cerberus_enrollment_token tok_...
/etc/init.d/S99cerberus restart
```

Older instructions that download `S99cerberus` from the release get a 404: the file is no longer
published. Run `cerberus init-script` instead, as above.

A token minted in the panel for one camera binds the camera to whoever minted it. With a batch token
the agent prints a claim code (also shown by `cerberus status`) that is entered in the panel to claim
the camera.

## Releases

There is one release, [`latest`](https://github.com/OpenIPC/cerberus/releases/latest), titled
`Cerberus <version>`; it is published again whenever the version changes. It carries one **static** binary per CPU architecture — static with musl and mbedTLS
inside, so it does not matter which libc or which mbedTLS a given firmware has, or whether it has one
at all:

| file               | runs on                                                                                                              |
|--------------------|----------------------------------------------------------------------------------------------------------------------|
| `cerberus-arm`     | every ARM camera from ARMv5 to ARMv7 — HiSilicon, Goke, SigmaStar, Fullhan, … (soft-float, so ARMv5 without VFP too) |
| `cerberus-mipsel`  | MIPS cameras — Ingenic T10, T20, T21, T31, T40, …                                                                    |
| `cerberus-aarch64` | 64-bit ARM                                                                                                           |
| `cerberus-x86_64`  | VMs and test boxes                                                                                                   |
| `install.sh`       | the installer; `--uninstall` takes the agent off again                                                               |
| `uninstall.sh`     | the old address of `install.sh --uninstall`, kept for commands already printed                                       |
| `SHA256SUMS`       | checksums of all of the above; the installer verifies against it                                                     |
| `LICENSE.md`       | the license                                                                                                          |

Asset names carry no version on purpose: `releases/latest/download/<name>` always points at the
current release.

The binaries of every new version are also posted to the OpenIPC Telegram channel ([our channels](https://openipc.org/our-channels)), for the archive and test downloads.

The change history of every version is in [CHANGELOG.md](CHANGELOG.md).

| binary             | bytes   |
|--------------------|---------|
| `cerberus-arm`     | 304 300 |
| `cerberus-mipsel`  | 397 772 |
| `cerberus-aarch64` | 329 240 |
| `cerberus-x86_64`  | 312 904 |

Most of that is mbedTLS, built with only what a TLS client needs (`ci/build.sh` lists what is left
out, and checks TLS against local servers on every build); the agent itself is ~30 KB.

## What the camera needs

| needed                      | for                             | without it                                                          |
|-----------------------------|---------------------------------|---------------------------------------------------------------------|
| kernel WireGuard + `wg`     | the whole data plane            | the agent is useless                                                |
| `ip`                        | address and link on `wg0`       | same                                                                |
| `fw_setenv` / `fw_printenv` | storage that survives a reflash | falls back to a file (which does not)                               |
| `ping`                      | the in-tunnel probe             | the probe is switched off with a warning; the handshake watch stays |
| `reboot`                    | the `Reboot` command            | the command is logged as impossible                                 |
| `curl` with TLS             | the installer                   | download the files by hand                                          |

## Configuration

`S99cerberus` reads the U-Boot environment and hands it to the agent as `CERBERUS_*` variables:

| U-Boot key                  | variable                    | default                                           | meaning                                                                   |
|-----------------------------|-----------------------------|---------------------------------------------------|---------------------------------------------------------------------------|
| `cerberus_api`              | `CERBERUS_API`              | —                                                 | coordinator base URL, including `/v1`                                     |
| `cerberus_enrollment_token` | `CERBERUS_ENROLLMENT_TOKEN` | —                                                 | one-shot token from the owner                                             |
| `cerberus_hostname`         | `CERBERUS_HOSTNAME`         | —                                                 | the name the camera shows up under in the panel                           |
| `cerberus_ca`               | `CERBERUS_CA`               | `/etc/ssl/certs/ca-certificates.crt`              | CA file or directory for TLS                                              |
| `cerberus_iface`            | `CERBERUS_IFACE`            | `wg0`                                             | WireGuard interface                                                       |
| `cerberus_subnet`           | `CERBERUS_SUBNET`           | `10.80.0.0/16`                                    | fallback only: the network's subnet arrives with the join material        |
| `cerberus_heartbeat`        | `CERBERUS_HEARTBEAT`        | `20`                                              | seconds between heartbeats; `0` disables                                  |
| `cerberus_ping_target`      | `CERBERUS_PING_TARGET`      | the hub                                           | in-tunnel probe target                                                    |
| `cerberus_ping_interval`    | `CERBERUS_PING_INTERVAL`    | `60`                                              | seconds between probes; `0` disables                                      |
| `cerberus_ping_timeout`     | `CERBERUS_PING_TIMEOUT`     | `3`                                               | seconds one probe waits                                                   |
| `cerberus_ping_fails`       | `CERBERUS_PING_FAILS`       | `3`                                               | failed probes in a row before the interface is rebuilt                    |
| `cerberus_events`           | `CERBERUS_EVENTS`           | `1`                                               | motion events; `0` installs no hook and sends nothing                     |
| `cerberus_snapshot_url`     | `CERBERUS_SNAPSHOT_URL`     | `http://127.0.0.1/image.jpg?width=640&height=360` | majestic's frame; `http://user:pass@127.0.0.1/…` when it wants a password |

`cerberus_ca` matters more than it looks: a firmware may carry no CA bundle at all, and then
enrolment fails on the coordinator's certificate with no way to fix it short of a reflash — point it
at the root certificate (ISRG Root X1 for Let's Encrypt).

Variables only the agent reads: `CERBERUS_STORE` (`auto` \| `uboot` \| `file:/path`),
`CERBERUS_STALE_AFTER` (`180`, seconds without a handshake before the peer is re-poked),
`CERBERUS_RETRY_MIN` / `CERBERUS_RETRY_MAX` (`5` / `300`, enrolment backoff), `CERBERUS_INSECURE=1`
(skip TLS verification, debugging only), `CERBERUS_DRY_RUN=1` (print the `ip`/`wg` commands instead
of running them), `CERBERUS_MOTION_HOOK` (`/usr/sbin/motion.sh`), `CERBERUS_MOTION_QUIET` (`10`,
seconds without motion that end an episode), `CERBERUS_MOTION_MAX` (`600`, an episode is cut after
this many seconds, whatever happens), `CERBERUS_PIDFILE` (`/var/run/cerberus.pid`),
`CERBERUS_SEED` (`/etc/cerberus/seed`, randomness kept between starts so that a start never waits
on the kernel), `CERBERUS_RANDOM_DEV` (`/dev/urandom`), `CERBERUS_GETRANDOM=0` (do not ask
getrandom(2) first).

```
cerberus run       # enroll + up + watch (the default; what S99cerberus starts)
cerberus enroll    # enroll only, idempotent
cerberus up        # bring the interface up from the stored material
cerberus status    # what is stored, handshake age, whether the tunnel answers
cerberus keygen    # print the keypair (stdout is data, diagnostics go to stderr)
cerberus clean     # remove the agent: interface, init script, stored material, the binary
cerberus motion    # tell the running agent there is motion, as majestic's hook does
cerberus snapshot  # fetch one frame from CERBERUS_SNAPSHOT_URL into /tmp/cerberus-snapshot.jpg
cerberus init-script [PATH|-]  # write the init script that starts this binary [/etc/init.d/S99cerberus]
```

### Formerly `streq_*`

Before this repository the keys were called `streq_*` and the variables `STREQ_*`. Cameras installed
then keep working, with nothing to do:

- on start the agent copies its own keys — device key, claim code, join material — from `streq_*`
  to `cerberus_*`, checks each copy, and from then on writes only the new names;
- the `streq_*` copies are **left in place**, so an older `S99cerberus`, or an older binary someone
  rolls back to, still finds a working camera;
- `S99cerberus` reads `cerberus_<name>` and falls back to `streq_<name>`, so settings written by the
  old instructions still apply; `STREQ_*` variables are still accepted;
- forgetting an enrolment (a revoked node), `cerberus clean` and `install.sh --uninstall` remove both names, so
  an old copy can never bring a revoked node back;
- once there is nothing left to copy, a start writes nothing to flash.

## How it works

### What lives where

Device key, node id, overlay address, hub endpoint and key, allowed IPs, the network's subnet, the
hub's overlay address and the claim code — all in the U-Boot environment.

**The key is there too, deliberately.** On a camera `/etc` is squashfs or an overlay on a partition a
reflash overwrites, while the node id in the U-Boot env survives it. A camera that kept its node id
and lost its key would come back with an identity the hub knows and no key to prove it — and
enrolment is skipped whenever a node id is stored. Key and node id have to live and die together.
`wg set private-key` takes a path, so for exactly one call the key is written to `/tmp` (0600) and
deleted straight after; it never touches the rootfs.

### The control plane: REST

Full nodes hold a SignalR connection to the coordinator. A C client for that is not this kind of
program, so the camera uses the REST twins instead: a heartbeat every `CERBERUS_HEARTBEAT` seconds,
and the answer to that heartbeat is its whole back-channel:

```jsonc
{
  "command": "None",          // None | Restart | Reboot — one-shot, delivered exactly once
  "join": {                   // the current join material, always whole, never a delta
    "kind": "Wireguard", "overlayIp": "10.80.0.7",
    "hubEndpoint": "hub.example:51820", "hubPublicKey": "base64…", "allowedIps": "10.80.0.7/32"
  },
  "mapVersion": 42
}
```

The agent compares `join` with what it has stored and applies **only the difference**: when the hub
moves or changes its key, the camera rewrites its store and reprograms the interface by itself.
`Restart` re-execs the agent; `Reboot` is `sync` and `reboot`. A `404` on the heartbeat means the
coordinator no longer knows the node: the agent drops its enrolment and stops, so the next start
enrols afresh instead of knocking with a dead node id forever.

### Supervising the tunnel

A fresh handshake does not mean a working tunnel: WireGuard re-handshakes every two minutes while
nothing crosses it — a route someone took, an interface somebody else reused, a hub that forwards
nothing. Only a packet that comes back says the tunnel works. The recovery ladder goes up in cost,
and the camera climbs it alone:

| what is seen                                     | what is done                                                 |
|--------------------------------------------------|--------------------------------------------------------------|
| handshake older than `CERBERUS_STALE_AFTER`      | the peer is re-poked (which also re-resolves the hub's name) |
| `CERBERUS_PING_FAILS` probes in a row unanswered | the interface is torn down and rebuilt                       |
| twice that — the rebuild did not help            | the agent re-execs itself                                    |

`cerberus run` does not die of an unreachable coordinator either: init on a camera starts it once and
never looks again, so enrolment is retried with a doubling pause for as long as it takes.

Video does not pass through the agent: the relay pulls `rtsp://{overlayIp}:554/stream0` straight from
the camera through the tunnel. There is no peer-to-peer on this agent — the camera stays on the hub
path, which is the path that always works.

### Motion events

majestic notices motion itself once its motion detection is switched on (web UI → Majestic →
Motion detection), and runs `/usr/sbin/motion.sh`. cerberus installs that script — keeping one that
was already there as `motion.sh.orig` and still calling it — and the script does nothing but wake
the agent. The agent turns the calls into episodes (motion, then `CERBERUS_MOTION_QUIET` seconds of
quiet) and reports each one twice, `started` and `ended`, to `POST /nodes/{id}/events`.

When the owner has switched object detection on for the camera, the answer to `started` asks for
frames: one a second from majestic's own `/image.jpg`, until the cloud says it has seen enough. The
cloud looks for people, cars and animals in them and tells the owner — the camera only says "look
now", from the moment it happens, without the cloud pulling the stream or knowing the RTSP password.

Events are signed. The heartbeat's answer carries the coordinator's `eventKey`; the key is
`HKDF-SHA256(X25519(device key, eventKey), salt = nodeId, info = "streq-events-v1")` and each request
carries `X-Streq-Ts` and `X-Streq-Sig = base64(HMAC-SHA256(key, ts \n nodeId \n scope \n body))`. No new
secret is stored on the camera: it lives exactly as long as the device key.

To check it on a camera: `cerberus snapshot` (does majestic hand out a frame as configured?) and
`cerberus motion` (an episode, as if somebody walked by).

## Building

The agent is the `src/` directory: `src/cerberus.c` and its `Makefile` — nothing else is needed to
build or install it. The top-level `Makefile` forwards to it. There is no init script among the
files: the binary carries it and writes it (`cerberus init-script`), so a script can never belong to
another release than the agent it starts.

```sh
make                                   # native, against the system mbedTLS (3.x or 2.28 LTS)
make STATIC=1                          # static
make VERSION=1.2.0                     # the version the agent reports (default: the one in src/cerberus.c)
make install DESTDIR=/tmp/rootfs       # /usr/sbin/cerberus + /etc/init.d/S99cerberus, written by it
```

The release binaries come from `ci/build.sh`, which needs only Docker: it fetches a
[Bootlin](https://toolchains.bootlin.com) musl toolchain and the mbedTLS source (both pinned by
sha256), builds statically, asserts the ELF machine, and then **runs** each binary under QEMU and
checks its X25519 key against `wg pubkey`:

```sh
ci/build.sh                            # arm, mipsel, aarch64, x86_64 → dist/cerberus-<arch>
ci/build.sh mipsel                     # one of them
ci/assemble.sh                         # + installers, SHA256SUMS → a release in dist/
```

Inside a firmware build, with the firmware's own toolchain, point at `src/`:

```sh
make -C src CC=mipsel-openipc-linux-musl-gcc MBEDTLS=<buildroot-staging> STATIC=1
make -C src install DESTDIR=<target-rootfs> RUN=<host-dir>/bin/qemu-mipsel
```

`make install` gets the init script by running the binary it built, which a cross build cannot do by
itself: give it a user-mode emulator in `RUN` (Buildroot's `host-qemu`; a static binary needs no
sysroot), or pass `INIT_SCRIPT=no` and have the firmware run `cerberus init-script` on the camera.
With neither it stops instead of installing an agent that nothing starts.

## Tests

```sh
make test                                                  # Alpine: musl + mbedTLS 3.x
CERBERUS_TEST_IMAGE=debian:bookworm-slim tests/run.sh      # glibc + mbedTLS 2.28
```

The suite needs only Docker. It builds the agent with `-Werror` and runs it against a fake
coordinator over real TLS: identity, TLS verification, enrolment, the back-channel (commands, a moved
or re-keyed hub, an outage, a revoked node), start-up backoff, the `streq_*` migration against a fake
U-Boot environment, the installer end to end from a release laid out on disk — and a real kernel
WireGuard tunnel to a hub in its own network namespace, including a stolen route the in-tunnel probe
has to notice and repair. The data-plane part needs kernel WireGuard on the host; `REQUIRE_WG=1`
makes its absence a failure, which is how CI runs it.

## CI and releases

Every push and pull request runs shellcheck, the suite on both mbedTLS generations, and the four
release builds. When a push to `master` brings a new `CERBERUS_VERSION` (`src/cerberus.c`), CI
replaces the one release, `latest`, with those same binaries, the installers, the license and
`SHA256SUMS`, titled `Cerberus <version>`; a push without a new version publishes nothing. The
version compiled into the binaries is `CERBERUS_VERSION` itself; there are no `v*` tags. To release:
bump the version and add its `CHANGELOG.md` section in the pull request, then merge it.

Versions are `MAJOR.MINOR.PATCH` (`CERBERUS_VERSION` in `src/cerberus.c`): PATCH for fixes and
small changes, MINOR for a new feature, MAJOR for an incompatible change or a large update, only
when the maintainer decides. A pull request that changes `src/`, the installers, the `Makefile` or
`ci/` raises the version as its last commit before the merge, together with its `CHANGELOG.md`
section; docs, tests and the workflows in `.github/` do not. CI checks this on every pull request
(`.github/check-version.sh`).

A change to the code or the build that leaves the binaries as they are (a build-script
tidy-up, a comment) can carry the `no-release` label instead: no new version, no release.
There are no versioned releases to pin: a rollback is a revert plus a new version.

## License

[The Prosperity Public License 3.0.0](LICENSE.md): free for noncommercial use, with a 30-day trial
for commercial use. See [LICENSE.md](LICENSE.md) for the full terms.

## Support

- Questions and bug reports: [GitHub Issues](https://github.com/OpenIPC/cerberus/issues)
- Documentation: [openipc.org](https://openipc.org)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
