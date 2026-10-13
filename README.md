# cerberus

[Русская версия](README.ru.md)

The cloud agent for OpenIPC cameras: a small C daemon that lives **inside the camera firmware** and
puts the camera on a private WireGuard overlay, so it can be reached from anywhere — video, ONVIF,
settings and SSH — with nothing between it and the cloud but the tunnel.

It does five things and nothing else:

1. generates the device's X25519 keypair and a claim code — once, on first start;
2. enrols at the coordinator over TLS (`POST /enroll`) and takes the join material;
3. keeps key, claim code and join material in the **U-Boot environment** via `fw_setenv` (a file is
   the fallback) — the one place on a camera that survives a firmware reflash;
4. brings up `wg0` from it and supervises the tunnel from both ends: the handshake age from `wg`
   **and** an ICMP probe of the hub *inside* the tunnel — a fresh handshake over a dead path looks
   alive;
5. reports liveness with `POST /nodes/{id}/heartbeat` and applies what comes back in the answer:
   changed join material and one-shot commands.

**mbedTLS** (TLS, X25519, SHA-256, base64, CTR-DRBG) is the only dependency; everything else is libc.
It drives the kernel WireGuard the firmware already ships, so it is tens of kilobytes rather than a
userspace WireGuard implementation.

## Installing on a camera

The usual way is one line over SSH, with an enrolment token from the cloud panel:

```sh
curl -fsSL https://cloud.openipc.ru/install.sh | sh -s -- tok_...
```

The installer (`bootstrap.sh` here) asks the camera for its processor (`fw_printenv -n soc`), fetches
the matching build, installs `/usr/sbin/cerberus` and `/etc/init.d/S99cerberus`, writes the address
and the token into the U-Boot environment and starts the agent. On failure it prints a short code —
`cerberus: E13` — and the page that decodes it:
<https://cloud.openipc.ru/docs/openipc-camera/#codes>. `uninstall.sh` takes all of it off again.

By hand, from a release tarball:

```sh
tar xzf cerberus-<version>-linux-musl-armv7.tar.gz && cd cerberus-<version>-linux-musl-armv7
./install.sh                 # /usr/sbin/cerberus + /etc/init.d/S99cerberus
fw_setenv streq_api https://cloud-api.openipc.ru/v1
fw_setenv streq_enrollment_token tok_...
reboot                       # or: /etc/init.d/S99cerberus restart
```

The init script is BusyBox, not systemd: `S99cerberus` reads its config out of the U-Boot env and
starts `cerberus run` with `start-stop-daemon`. Updating is replacing the binary and restarting the
service; key, node id and claim code live in the U-Boot env and survive it.

A token minted in the panel for one camera binds the camera to whoever minted it. With a batch token
the agent prints a claim code (also shown by `cerberus status`) that is entered in the panel to
claim the camera. Until it is claimed the camera is on the overlay, but nobody can watch or command
it.

### What the camera needs

| needed | for | without it |
|---|---|---|
| kernel WireGuard + `wg` | the whole data plane | the agent is useless |
| `ip` | address and link on `wg0` | same |
| `fw_setenv` / `fw_printenv` | storage that survives a reflash | falls back to a file (which does not) |
| shared mbedTLS | TLS, X25519, SHA-256 | use a static build |
| `ping` | the in-tunnel probe | the probe is switched off with a warning; the handshake watch stays |
| `reboot` | the `Reboot` command | the command is logged as impossible |

## Building

```sh
make                                   # native, against the system mbedTLS
make STATIC=1                          # static
make VERSION=1.2.0                     # the version the agent reports (default: the one in main.c)
make install DESTDIR=/tmp/rootfs       # /usr/sbin/cerberus + /etc/init.d/S99cerberus
```

It builds against both mbedTLS generations in the field: 3.x and 2.28 LTS — the latter is what
Buildroot, and so most OpenIPC-era firmware, carries.

Release tarballs are built in containers; the host needs only Docker:

```sh
ci/build.sh musl-armv7                 # OpenIPC on ARMv7: static musl, no dependencies at all
ci/build.sh armhf arm64 amd64          # generic SBCs (glibc, dynamic)
                                       # → dist/cerberus-<version>-linux-<arch>.tar.gz (+ .sha256)
```

**A glibc build does not start on OpenIPC at all.** The firmware is musl: a glibc binary asks for a
loader (`/lib/ld-linux-armhf.so.3`) that is not there, and the shell answers `not found` about a file
that is right in front of you. Hence the separate `musl-armv7` build. It runs inside an emulated
armv7 Alpine (binfmt/QEMU — Docker Desktop has it; on plain Linux run
`docker run --privileged --rm tonistiigi/binfmt --install arm` once), so no cross toolchain is
needed: the libc is the one the firmware runs. It is static, with mbedTLS inside, so it does not
matter which mbedTLS a given firmware carries, or whether it carries one at all.

Check the camera before picking a build:

```sh
uname -m                                    # armv7l → musl-armv7 fits; mips/mipsel → it does not
grep -i features /proc/cpuinfo | head -1    # needs vfp: ARMv5 without it cannot run armv7 code
ls /lib/ld-musl-* 2>/dev/null               # confirms musl firmware
ls /usr/lib/libmbedtls* 2>/dev/null         # firmware has mbedTLS → can link dynamically
```

Ingenic T20/T31 are MIPS, and ARMv5 SoCs have no VFP; both need the firmware's own Buildroot
toolchain:

```sh
make CC=mipsel-openipc-linux-musl-gcc MBEDTLS=<buildroot-staging> STATIC=1
# or, when the firmware has a matching mbedTLS, without STATIC=1 — tens of KB instead of hundreds
```

`STATIC=1` is only sane with musl: static glibc still needs the shared NSS libraries at runtime for
`getaddrinfo` — exactly the call that resolves the hub.

### Size

`-Os`, `--gc-sections`, stripped; measured from the `ci/build.sh` tarballs:

| target | bytes |
|---|---|
| armv7, musl, static — OpenIPC | 538 080 |
| armv7, glibc, dynamic (armhf) | 26 568 |
| arm64, glibc, dynamic | 68 568 |
| x86-64, glibc, dynamic | 44 120 |

The agent itself is the dynamic armv7 number. Nearly all of the static build is mbedTLS as Alpine
builds it — server, DTLS, RSA and every cipher suite. Firmware builds mbedTLS with its own config
(TLS client, ECDHE + AEAD, X.509), and a static agent against that is a fraction of the size.

At runtime it is one process, one thread, about 1.4 MB RSS — mostly mbedTLS buffers during the TLS
handshake. Each heartbeat is a new TLS connection, ~4 KB on the wire, so the default 20 seconds is
~17 MB a day; `STREQ_HEARTBEAT=60` divides that by three on a metered link, at the price of commands
and rotations arriving that much later.

## Tests

```sh
make test                                                  # Alpine: musl + mbedTLS 3.x
CERBERUS_TEST_IMAGE=debian:bookworm-slim tests/run.sh      # glibc + mbedTLS 2.28
```

The suite needs only Docker. It builds the agent with `-Werror` and runs it against a fake
coordinator (`tests/fake_coordinator.py`) over real TLS with its own CA:

- **identity** — the X25519 key is the one `wg pubkey` derives, it lives in the store and nowhere
  else, the store is 0600, a legacy key file is migrated in unchanged;
- **TLS** — an unrelated CA and a certificate for another name are both refused before a byte of
  the request leaves;
- **enrolment** — the request carries the public key and `sha256(claim code)`, never the code
  itself; the join material is persisted; a second enrol spends nothing;
- **the back-channel** — `Restart` re-execs the agent, `Reboot` reboots, unknown commands are
  ignored, a moved or re-keyed hub is picked up and the interface reprogrammed, a coordinator outage
  is one warning and is survived, a revoked node discards its enrolment and stops;
- **start-up** — an unreachable coordinator is waited out with backoff, and SIGTERM during the
  pause is obeyed at once;
- **the data plane** — a real kernel WireGuard tunnel to a hub in its own network namespace:
  handshake, ping through the tunnel, and a stolen route noticed by the in-tunnel probe and repaired
  by rebuilding the interface;
- **the shipped scripts** parse, and the installers carry no comments.

The data-plane part needs kernel WireGuard on the host (any current Linux, Docker Desktop's VM) and
is skipped without it; `REQUIRE_WG=1` makes that a failure instead, which is how CI runs it.

## CI and releases

`.github/workflows/ci.yml` runs shellcheck, the suite on both mbedTLS generations, and builds the
four tarballs on every push and pull request. Pushing a `v*` tag publishes those same tarballs, with
their `.sha256` files, as a GitHub release; the version compiled into the binary comes from the tag.

```sh
git tag v0.2.0 && git push origin v0.2.0
```

## Running

Configuration is `STREQ_*` environment variables; `S99cerberus` fills them from the U-Boot env.

| variable | default | meaning |
|---|---|---|
| `STREQ_API` | `https://api.localhost/v1` | coordinator base URL, including `/v1` |
| `STREQ_ENROLLMENT_TOKEN` | — | one-shot token from the owner |
| `STREQ_KIND` | `Camera` | node kind |
| `STREQ_HOSTNAME` | — | the name the camera shows up under in the panel |
| `STREQ_IFACE` | `wg0` | WireGuard interface |
| `STREQ_SUBNET` | `10.80.0.0/16` | fallback: the network's subnet and the hub's address arrive with the join material |
| `STREQ_CA` | `/etc/ssl/certs/ca-certificates.crt` | CA file or directory for TLS |
| `STREQ_INSECURE` | `0` | skip TLS verification (debugging only) |
| `STREQ_STORE` | `auto` | `auto` \| `uboot` \| `file:/path` — the device key lives here too |
| `STREQ_KEYFILE` | `/etc/streq/wg.key` | legacy key location; migrated into the store on sight |
| `STREQ_KEY_TMP` | `/tmp/.streq-wg.key` | where the key is materialised for one `wg set` call |
| `STREQ_HEARTBEAT` | `20` | seconds between heartbeats; `0` disables |
| `STREQ_PING_TARGET` | the hub | in-tunnel probe target |
| `STREQ_PING_INTERVAL` | `60` | seconds between probes; `0` disables |
| `STREQ_PING_TIMEOUT` | `3` | seconds one probe waits |
| `STREQ_PING_FAILS` | `3` | consecutive failed probes before the interface is rebuilt |
| `STREQ_STALE_AFTER` | `180` | seconds without a handshake before the peer is re-poked |
| `STREQ_RETRY_MIN` / `STREQ_RETRY_MAX` | `5` / `300` | enrolment retry backoff, seconds |
| `STREQ_DRY_RUN` | `0` | print the `ip`/`wg` commands instead of running them |

The U-Boot keys `S99cerberus` reads: `streq_api`, `streq_enrollment_token`, `streq_hostname`,
`streq_ca`, `streq_subnet`, `streq_ping_target`, `streq_ping_interval`, `streq_iface` (all but the
first two optional; empty means default). `streq_ca` matters more than it looks: a firmware may
carry no CA bundle at all, and then enrolment fails on the coordinator's certificate with no way to
fix it short of a reflash — point it at the root certificate (ISRG Root X1 for Let's Encrypt).

```
cerberus run       # enroll + up + watch (the default; what S99cerberus starts)
cerberus enroll    # enroll only, idempotent
cerberus up        # bring the interface up from the stored material
cerberus status    # what is stored, handshake age, whether the tunnel answers
cerberus keygen    # print the keypair (stdout is data, diagnostics go to stderr)
cerberus clean     # remove the agent: interface, init script, stored material, the binary
```

## What lives where

`streq_device_key`, `streq_node_id`, `streq_overlay_ip`, `streq_hub_endpoint`, `streq_hub_pubkey`,
`streq_allowed_ips`, `streq_network_subnet`, `streq_hub_ip`, `streq_claim_code` — all in the U-Boot
environment.

**The key is there too, deliberately.** A separate 0600 file would be the obvious place, but on a
camera `/etc` is squashfs or an overlay on a partition a reflash overwrites, while `streq_node_id`
next to it in the U-Boot env survives. The node would come back with an identity the hub has a peer
for and no key to prove it — and enrolment is skipped whenever a node id is stored. That is not
"re-enrols", it is *stuck for good*. Key and node id have to live and die together, so they share a
store. `wg set private-key` takes a path, so for exactly one call the key is materialised in `/tmp`
(0600) and deleted straight after; it is never written to the rootfs.

`streq_network_subnet` and `streq_hub_ip` arrive with enrolment: every network on a multi-tenant
instance has its own range, and the node takes its address mask and a separate route to the hub
from them. The route matters because WireGuard's `allowed-ips` is cryptokey routing, not the
kernel's routing table: without it the handshake succeeds and the packets go around the tunnel.

## The control plane: REST, not the hub

Full nodes hold a SignalR connection to the coordinator and get the network map pushed to them. A C
client for that is not this kind of program, so thin nodes take the degraded path:

| hub | what cerberus does |
|---|---|
| `Heartbeat(nodeId)` | `POST /nodes/{id}/heartbeat` every `STREQ_HEARTBEAT` seconds |
| `DirectiveIssued` (push) | the answer to that same heartbeat — the same `NodeDirective` |
| `JoinNetwork(nodeId)` → map | `GET /nodes/{id}/map` exists, but the agent does not poll it |
| `NetworkMapUpdated` / `PathRevoked` | no pushes |

A camera does not need the map: it has exactly one peer, the hub, and that arrives with the join
material. A `404` on the heartbeat means the coordinator no longer knows the node (revoked or
rebuilt): the agent drops the enrolment from the store and stops, so the next start enrols afresh
instead of knocking with a dead node id forever.

### The back-channel: the heartbeat answer

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
moved (a new VPS, a new DNS name) or changed its key, the camera rewrites its store and reprograms
the interface by itself. `Restart` re-execs the agent (there may be no supervisor on the camera);
`Reboot` is `sync` and `reboot`.

### Supervising the tunnel

The handshake and the probe answer different questions, and neither follows from the other:
WireGuard re-handshakes about every two minutes and shows a perfectly fresh one while nothing at all
crosses the tunnel — a route someone took, an interface somebody else reused, a hub that completes
handshakes and forwards nothing. The handshake says the peer is there; only a packet that comes back
says the tunnel works.

The recovery ladder goes up in cost, and the camera has to climb it alone:

| what is seen | what is done |
|---|---|
| handshake older than `STREQ_STALE_AFTER` | the peer is re-poked (which also re-resolves the hub's name) |
| `STREQ_PING_FAILS` probes in a row unanswered | the interface is torn down and rebuilt |
| twice that — the rebuild did not help | the agent re-execs itself |

A camera that reboots over one lost packet is worse than one that just stays down, so thresholds
count runs, not events.

Start-up is separate. `cerberus run` **does not die of an unreachable coordinator**: enrolment is
retried with a doubling pause from `STREQ_RETRY_MIN` to `STREQ_RETRY_MAX`, as long as it takes. Init
on a camera starts the agent once and never looks at it again, so exiting there would mean a camera
lost until the next power cycle — over a network, DNS or NTP that came up a few seconds late.
Configuration errors still exit at once: retrying does not invent a token nobody wrote down.

## What it does not do

No pushes and no peer-to-peer. `PathRevoked` only travels over the hub; a thin node picks a
revocation up from the next heartbeat. Hole-punching goes through the hub (`ReportCandidates` →
`PunchNow` → `ReportPathState`) and needs a simultaneity polling cannot give, so this agent stays on
the hub path — which is exactly the path that always works.

Video does not pass through the agent at all: the relay pulls `rtsp://{overlayIp}:554/stream0`
straight from the camera through the tunnel.

## License

[The Prosperity Public License 3.0.0](LICENSE.md), © OpenIPC. Free for noncommercial use —
personal, hobby, research, and noncommercial organizations — and a thirty-day trial for commercial
use; commercial use beyond the trial is not covered by this license. Every copy, binaries included,
carries the license text with it, which is why the release tarballs ship `LICENSE.md`.
