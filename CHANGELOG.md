# Changelog

The version lives in `CERBERUS_VERSION` in `src/cerberus.c` (`MAJOR.MINOR.PATCH`). Binaries are
published only as the [`latest`](https://github.com/OpenIPC/cerberus/releases/latest) release
(see the README); this file is the change history, newest first.

## 0.3.1 — 2026-10-17

One release, `latest` (#32), and a version check (#33).

- Releases: one release, `latest`, titled `Cerberus <version>`, published from `master` when
  `CERBERUS_VERSION` changes; the versioned releases `v0.1.0`…`v0.3.0` and their tags are gone,
  their notes are in this file. The download links (`releases/latest/download/<name>`) and the
  files stay the same.
- The version compiled into the binary is `CERBERUS_VERSION` in `src/cerberus.c`; it no longer
  comes from a `v*` tag.
- CI: a pull request that changes the code or the build raises the version and adds its section
  here (#33).

## 0.3.0 — 2026-10-15

- Review fixes: the snapshot URL, the probe subnet, the enrolment JSON, `--help` (PR #12).
- The agent moved into `src/`: `src/cerberus.c` with its own Makefile, so an external build can
  point at it alone (PR #15).
- Release builds link an mbedTLS trimmed to a TLS client, about 45 % smaller, with a TLS check
  per binary (PR #19, #17, #18).
- `cerberus init-script [PATH|-]` writes the BusyBox init script; the installer has the agent
  write `/etc/init.d/S99cerberus` instead of downloading a copy (PR #20, #22).
- One installer: `install.sh --uninstall` takes the agent off; `uninstall.sh` stays as the old
  address of that command (PR #21).

## 0.2.2 — 2026-10-14

- The first frame is taken at the moment of motion, not after the cloud has answered (PR #5).

## 0.2.1 — 2026-10-14

- No more minutes of waiting on the kernel's randomness at start: the random generator is
  seeded on first use only (PR #4).

## 0.2.0 — 2026-10-14

- Motion events: the agent tells the cloud when majestic sees movement, with frames; the motion
  hook only wakes the agent, and a hook that was already there is kept (PR #3).

## 0.1.0 — 2026-10-13

- First release: the cerberus agent (PR #1, #2).
