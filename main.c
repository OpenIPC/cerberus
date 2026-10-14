/*
 * cerberus — узловой агент для камеры (C-вариант C1, для прошивки OpenIPC).
 *
 * On-camera control agent: the smallest thing that can put a camera into the Streq overlay when the
 * firmware already ships kernel WireGuard and `wg`. It does five things and nothing else:
 *
 *   1. generates a device X25519 keypair (mbedTLS) and a claim code, once;
 *   2. POSTs /enroll to the coordinator over TLS (mbedTLS) and takes the join material;
 *   3. persists key, claim code and join material into the U-Boot environment via fw_setenv (file
 *      store as fallback) — the one place on the camera a firmware update does not wipe;
 *   4. programs wg0 from it and supervises the tunnel: the handshake age from `wg`, and an ICMP probe
 *      of the hub *inside* the tunnel — a fresh handshake over a dead path is a live-looking corpse;
 *   5. posts liveness to /nodes/{id}/heartbeat — the REST half of the control plane, for nodes that
 *      cannot hold a SignalR connection (the coordinator's "thin node" REST twins) — and applies the
 *      NodeDirective that comes back: rotated join material (the hub moved or re-keyed) and one-shot
 *      commands (Restart / Reboot). That answer is the only back-channel a thin node has.
 *   6. tells the cloud about motion: majestic runs a hook, the hook wakes us, and we post a signed
 *      episode (started / ended) plus, while it lasts and only if the owner switched detection on,
 *      a frame a second from majestic's own /image.jpg. What is in the frame — a person, a car — is
 *      decided by the cloud's object detector; the camera only says "look now".
 *
 * It is deliberately NOT the .NET agent: no SignalR, no map polling, no go2rtc republishing. A camera
 * that is itself on the overlay needs none of that — the relay pulls rtsp://{overlayIp}:554/stream0
 * straight off it, and its only peer, the hub, arrives with the join material.
 *
 * Everything here talks the coordinator's frozen REST contract (OpenAPI 3.1, served under /v1).
 */

#define _POSIX_C_SOURCE 200809L
/* sync(3) — XSI, and OpenIPC's musl gates it the same way glibc does. */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

/* Both mbedTLS generations are in the field: 3.x on current distros, 2.28 LTS in Buildroot (and so
 * in most OpenIPC-era firmware). The only call that differs is the one-shot SHA-256. */
#if MBEDTLS_VERSION_MAJOR < 3
#define sha256_once(in, len, out) mbedtls_sha256_ret((in), (len), (out), 0)
#else
#define sha256_once(in, len, out) mbedtls_sha256((in), (len), (out), 0)
#endif

/* Release builds pass the tag in (`make VERSION=...`); a plain `make` reports the source version. */
#ifndef CERBERUS_VERSION
#define CERBERUS_VERSION "0.2.1"
#endif

/* Where the errors are decoded. Printed after every fatal one: the reader is on a camera
 * console, with no repository, no log anyone else can see and often no idea what this binary
 * is. A wrong chip, a spent token and a firewall all look alike from there — the page is what
 * tells them apart, and the installer prints the same address for the same reason. */
#define CERBERUS_DOCS "https://cloud.openipc.ru/docs/openipc-camera/#codes"

/* Куда установщик кладёт автозапуск и сам агент: то же самое убирает `clean`. */
#define INIT_SCRIPT "/etc/init.d/S99cerberus"
#define DEFAULT_SELF_PATH "/usr/sbin/cerberus"

/* ---------------------------------------------------------------- logging */

static int g_quiet;

/* Diagnostics go to stderr, always: stdout belongs to whatever the subcommand prints as data
 * (`keygen` hands its keypair straight to `wg`, `status` gets scraped by init scripts). */
static void note(const char *fmt, ...)
{
    va_list ap;
    if (g_quiet) return;
    va_start(ap, fmt);
    fputs("cerberus: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static void warn_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("cerberus: warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("cerberus: error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fprintf(stderr, "cerberus: %s\n", CERBERUS_DOCS);
    exit(1);
}

/* ----------------------------------------------------------------- config */

/* Bound from CERBERUS_* env, same convention as the .NET agent (AgentOptions). */
struct cfg {
    const char *api;            /* coordinator base URL incl. /v1 */
    const char *token;          /* one-shot enrollment token */
    const char *kind;           /* NodeKind — Camera for an on-cam agent */
    const char *hostname;
    const char *iface;          /* wg interface to program */
    const char *subnet;         /* overlay CIDR routed to the hub */
    const char *ca;             /* CA bundle/dir for TLS verification */
    const char *store;          /* "auto" | "uboot" | "file:/path" */
    const char *keyfile;        /* legacy plain-file device key: migrated into the store on sight */
    const char *ping_target;    /* адрес внутри туннеля для ICMP-пробы; пусто → вывести из subnet */
    int insecure;               /* skip TLS verification (dev only) */
    int dry_run;                /* print the ip/wg commands instead of running them */
    int watch_interval;         /* seconds between handshake checks */
    int stale_after;            /* seconds without a handshake before re-poking the peer */
    int heartbeat;              /* seconds between liveness posts; 0 disables */
    int ping_interval;          /* seconds between in-tunnel ICMP probes; 0 disables */
    int ping_timeout;           /* seconds one probe waits for its reply */
    int ping_fails;             /* consecutive failed probes before the interface is rebuilt */
    int retry_min;              /* first pause before a failed enrolment is retried */
    int retry_max;              /* ceiling that pause backs off to */
    int events;                 /* report motion episodes; 0 disables the hook and the events */
    const char *motion_hook;    /* the script majestic runs on motion */
    const char *pidfile;        /* where the hook finds us */
    const char *snapshot_url;   /* majestic's JPEG, user:pass@ allowed */
    int snapshot_max;           /* largest frame we accept from majestic, bytes */
    int motion_quiet;           /* seconds without motion that end an episode */
    int motion_max;             /* an episode is cut after this many seconds, whatever happens */
    const char *seed;           /* randomness kept between starts, so a start never waits on the kernel */
    const char *random_dev;     /* the kernel's random device */
    int getrandom;              /* ask getrandom(2) first; 0 for a kernel whose getrandom misbehaves */
};

static struct cfg g_cfg;

/* CERBERUS_<name>, then the name this agent had before it was renamed — CERBERUS_<name> — so a script or an
 * init file written for the old name keeps working. */
static const char *cfg_env(const char *name, const char *fallback)
{
    char key[64];
    const char *v;

    snprintf(key, sizeof key, "CERBERUS_%s", name);
    v = getenv(key);
    if (v && *v) return v;
    snprintf(key, sizeof key, "STREQ_%s", name);
    v = getenv(key);
    return (v && *v) ? v : fallback;
}

static void cfg_load(void)
{
    g_cfg.api = cfg_env("API", "https://api.localhost/v1");
    g_cfg.token = cfg_env("ENROLLMENT_TOKEN", "");
    g_cfg.kind = cfg_env("KIND", "Camera");
    g_cfg.hostname = cfg_env("HOSTNAME", NULL);
    g_cfg.iface = cfg_env("IFACE", "wg0");
    g_cfg.subnet = cfg_env("SUBNET", "10.80.0.0/16");
    g_cfg.ca = cfg_env("CA", "/etc/ssl/certs/ca-certificates.crt");
    g_cfg.store = cfg_env("STORE", "auto");
    g_cfg.keyfile = cfg_env("KEYFILE", "/etc/streq/wg.key");
    g_cfg.insecure = atoi(cfg_env("INSECURE", "0"));
    g_quiet = atoi(cfg_env("QUIET", "0"));
    g_cfg.dry_run = atoi(cfg_env("DRY_RUN", "0"));
    g_cfg.watch_interval = atoi(cfg_env("WATCH_INTERVAL", "30"));
    g_cfg.stale_after = atoi(cfg_env("STALE_AFTER", "180"));

    /* The coordinator's contract: 20s by default, never faster than 5. */
    g_cfg.heartbeat = atoi(cfg_env("HEARTBEAT", "20"));
    if (g_cfg.heartbeat > 0 && g_cfg.heartbeat < 5) g_cfg.heartbeat = 5;
    if (g_cfg.watch_interval < 1) g_cfg.watch_interval = 1;

    /* The in-tunnel probe. Slower than the handshake check on purpose: it costs a packet round trip
     * over the overlay, and what it is looking for (a path that is up on paper and dead in fact) does
     * not appear between two ticks. */
    g_cfg.ping_target = cfg_env("PING_TARGET", "");
    g_cfg.ping_interval = atoi(cfg_env("PING_INTERVAL", "60"));
    g_cfg.ping_timeout = atoi(cfg_env("PING_TIMEOUT", "3"));
    g_cfg.ping_fails = atoi(cfg_env("PING_FAILS", "3"));
    if (g_cfg.ping_interval > 0 && g_cfg.ping_interval < 10) g_cfg.ping_interval = 10;
    if (g_cfg.ping_timeout < 1) g_cfg.ping_timeout = 1;
    if (g_cfg.ping_fails < 1) g_cfg.ping_fails = 1;

    /* Ждать координатор, а не умирать. См. enroll_with_retry. */
    g_cfg.retry_min = atoi(cfg_env("RETRY_MIN", "5"));
    g_cfg.retry_max = atoi(cfg_env("RETRY_MAX", "300"));
    if (g_cfg.retry_min < 1) g_cfg.retry_min = 1;
    if (g_cfg.retry_max < g_cfg.retry_min) g_cfg.retry_max = g_cfg.retry_min;

    /* Motion. The frame is majestic's: taken on the camera at the moment of motion, so the cloud
     * needs neither the stream nor the RTSP password — and gets it without the seconds a relay
     * spends bringing a stream up. 640 px wide is plenty for a person at the gate. */
    g_cfg.events = atoi(cfg_env("EVENTS", "1"));
    g_cfg.motion_hook = cfg_env("MOTION_HOOK", "/usr/sbin/motion.sh");
    g_cfg.pidfile = cfg_env("PIDFILE", "/var/run/cerberus.pid");
    g_cfg.snapshot_url = cfg_env("SNAPSHOT_URL", "http://127.0.0.1/image.jpg?width=640&height=360");
    g_cfg.snapshot_max = atoi(cfg_env("SNAPSHOT_MAX", "524288"));
    g_cfg.motion_quiet = atoi(cfg_env("MOTION_QUIET", "10"));
    g_cfg.motion_max = atoi(cfg_env("MOTION_MAX", "600"));
    if (g_cfg.snapshot_max < 16384) g_cfg.snapshot_max = 16384;
    if (g_cfg.motion_quiet < 1) g_cfg.motion_quiet = 1;
    if (g_cfg.motion_max < g_cfg.motion_quiet) g_cfg.motion_max = g_cfg.motion_quiet;

    g_cfg.seed = cfg_env("SEED", "/etc/cerberus/seed");
    g_cfg.random_dev = cfg_env("RANDOM_DEV", "/dev/urandom");
    g_cfg.getrandom = atoi(cfg_env("GETRANDOM", "1"));
}

/* ------------------------------------------------------------------ crypto */

/*
 * Randomness, and why a start must not wait for it.
 *
 * mbedTLS seeds itself from the kernel: getrandom(2), or /dev/urandom where the kernel predates it.
 * On some cameras that read blocks for minutes, and on every start, because each start drains what
 * the kernel had gathered and an idle camera gathers it slowly. It used to happen before any
 * command at all — `cerberus status` included — and after every restart the agent sat silent for
 * those minutes before it said a word to the cloud.
 *
 * Two things fix it. Nothing seeds until something actually needs randomness (TLS, a key, an
 * episode id): `status`, `snapshot`, `motion` and `clean` never touch it. And what seeds is not the
 * kernel alone: whatever it hands over right now without waiting, mixed with a seed kept from the
 * previous start and replaced on every one. Waiting on the kernel happens once in a device's life —
 * the very first start, when there is no seed yet and the kernel has nothing ready — never again.
 *
 * The seed file is as secret as the device key in the store, and no more: whoever can read one can
 * read the other.
 */
static mbedtls_ctr_drbg_context g_drbg;
static int g_drbg_ready;
static int g_seeded;    /* seeded once in this process: a reseed later never waits */

#define SEED_LEN 32
#define GRND_NONBLOCK_ 0x0001

#if defined(SYS_getrandom)
/* _POSIX_C_SOURCE hides syscall()'s prototype; it is the same in every libc. */
long syscall(long number, ...);
#endif

static void mkparent(const char *path);

/* Fills `out` from `path`, or fails whole. `nonblock` turns "would wait" into a failure. */
static int read_exact(const char *path, int nonblock, unsigned char *out, size_t len)
{
    size_t n = 0;
    int fd = open(path, O_RDONLY | (nonblock ? O_NONBLOCK : 0));

    if (fd < 0) return 0;
    while (n < len) {
        ssize_t r = read(fd, out + n, len - n);
        if (r > 0) { n += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        break;   /* EOF, EAGAIN (nothing ready) or a real error: all the same here */
    }
    close(fd);
    return n == len;
}

/* What the kernel has ready this instant, or nothing. */
static int kernel_random_now(unsigned char *out, size_t len)
{
#if defined(SYS_getrandom)
    if (g_cfg.getrandom && syscall(SYS_getrandom, out, len, GRND_NONBLOCK_) == (long)len)
        return 1;
#endif
    return read_exact(g_cfg.random_dev, 1, out, len);
}

static void seed_write(const unsigned char seed[SEED_LEN])
{
    static int said;
    char tmp[320];
    int fd, ok;

    snprintf(tmp, sizeof tmp, "%s.new", g_cfg.seed);
    mkparent(g_cfg.seed);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ok = fd >= 0 && write(fd, seed, SEED_LEN) == SEED_LEN;
    if (fd >= 0 && close(fd) != 0) ok = 0;
    if (ok && rename(tmp, g_cfg.seed) != 0) ok = 0;
    if (!ok) {
        if (fd >= 0) unlink(tmp);
        if (!said++)
            warn_("cannot keep the random seed in %s (%s) — every start asks the kernel again",
                  g_cfg.seed, strerror(errno));
    }
}

/* HMAC-SHA256 over two parts; defined with the event signing below. */
static int hmac256(const unsigned char *key, size_t key_len,
                   const unsigned char *a, size_t a_len,
                   const unsigned char *b, size_t b_len, unsigned char out[32]);

/*
 * The generator's entropy callback: kernel bytes that are ready now, the kept seed, the time and the
 * pid, hashed into as many bytes as asked; a counter keeps any two calls in a process apart. It
 * blocks in exactly one case — nothing strong at all, on the first seeding — and then on the kernel
 * alone, which is what mbedTLS used to do on every start.
 */
static int entropy_cb(void *unused, unsigned char *out, size_t len)
{
    static unsigned long calls;
    unsigned char pool[48 + SEED_LEN + 2 * sizeof(struct timespec) + sizeof(long)];
    unsigned char block[32], tag[sizeof(unsigned long) + 4];
    size_t n = 0, done = 0;
    int strong = 0;
    struct timespec ts;
    unsigned long ctr = ++calls;
    long pid = (long)getpid();

    (void)unused;

    if (kernel_random_now(pool, 48)) { n += 48; strong = 1; }
    if (read_exact(g_cfg.seed, 0, pool + n, SEED_LEN)) { n += SEED_LEN; strong = 1; }
    if (!strong && !g_seeded) {
        note("waiting for the kernel's randomness — once, the first time on this device");
        if (!read_exact(g_cfg.random_dev, 0, pool + n, 48)) return -1;
        n += 48;
    }
    clock_gettime(CLOCK_REALTIME, &ts);
    memcpy(pool + n, &ts, sizeof ts);
    n += sizeof ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    memcpy(pool + n, &ts, sizeof ts);
    n += sizeof ts;
    memcpy(pool + n, &pid, sizeof pid);
    n += sizeof pid;

    while (done < len) {
        size_t take = len - done < sizeof block ? len - done : sizeof block;
        uint32_t i = (uint32_t)(done / sizeof block);
        memcpy(tag, &ctr, sizeof ctr);
        memcpy(tag + sizeof ctr, &i, 4);
        if (hmac256(tag, sizeof tag, pool, n, NULL, 0, block) != 0) return -1;
        memcpy(out + done, block, take);
        done += take;
    }
    mbedtls_platform_zeroize(pool, sizeof pool);
    mbedtls_platform_zeroize(block, sizeof block);
    return 0;
}

static void crypto_init(void)
{
    const char *pers = "cerberus";
    unsigned char next[SEED_LEN];

    mbedtls_ctr_drbg_init(&g_drbg);
    if (mbedtls_ctr_drbg_seed(&g_drbg, entropy_cb, NULL, (const unsigned char *)pers, strlen(pers)) != 0)
        die("no entropy source");
    g_seeded = 1;
    g_drbg_ready = 1;

    /* The next start's seed comes out of the generator, so it says nothing about what this one makes. */
    if (mbedtls_ctr_drbg_random(&g_drbg, next, sizeof next) == 0)
        seed_write(next);
    mbedtls_platform_zeroize(next, sizeof next);

    /* After ours is seeded: in the release build PSA draws from it (mbedtls_psa_external_get_random
     * below) instead of gathering from the kernel a second time. TLS 1.3 needs PSA up. */
#if defined(MBEDTLS_PSA_CRYPTO_C)
    psa_crypto_init();
#endif
}

/* The generator, seeded on first use. */
static mbedtls_ctr_drbg_context *drbg(void)
{
    if (!g_drbg_ready) crypto_init();
    return &g_drbg;
}

#if defined(MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG)
/* The release build's mbedTLS is configured to ask the application for randomness (ci/build.sh). */
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *context,
                                             uint8_t *output, size_t output_size, size_t *output_length)
{
    size_t done = 0;

    (void)context;
    while (done < output_size) {
        size_t take = output_size - done;
        if (take > MBEDTLS_CTR_DRBG_MAX_REQUEST) take = MBEDTLS_CTR_DRBG_MAX_REQUEST;
        if (mbedtls_ctr_drbg_random(drbg(), output + done, take) != 0)
            return PSA_ERROR_INSUFFICIENT_ENTROPY;
        done += take;
    }
    *output_length = output_size;
    return PSA_SUCCESS;
}
#endif

static void b64(const unsigned char *in, size_t n, char *out, size_t out_sz)
{
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_sz, &olen, in, n) != 0)
        die("base64 buffer too small");
    out[olen] = 0;
}

static int b64_decode(const char *in, unsigned char *out, size_t out_sz, size_t *olen)
{
    return mbedtls_base64_decode(out, out_sz, olen, (const unsigned char *)in, strlen(in));
}

static void sha256_hex(const char *in, char out[65])
{
    unsigned char d[32];
    int i;
    if (sha256_once((const unsigned char *)in, strlen(in), d) != 0)
        die("sha256 failed");
    for (i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", d[i]);
    out[64] = 0;
}

/*
 * X25519 public key from a private key, the WireGuard way: clamp, then scalar-multiply the Curve25519
 * base point. mbedTLS carries the curve already (it is a TLS key exchange), so this costs no extra
 * dependency — the alternative would be shelling out to `wg pubkey`.
 */
static int x25519_pub(const unsigned char priv[32], unsigned char pub[32])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi d;
    size_t olen = 0;
    int rc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) rc = mbedtls_mpi_read_binary_le(&d, priv, 32);
    if (rc == 0) rc = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, mbedtls_ctr_drbg_random, drbg());
    /* For a Montgomery curve this writes X little-endian in 32 bytes — exactly WireGuard's
     * encoding. Going through the public API keeps us off mbedTLS 3.x private struct fields. */
    if (rc == 0) rc = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                                     &olen, pub, 32);
    if (rc == 0 && olen != 32) rc = -1;

    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

/*
 * X25519(priv, peer): the shared secret, little-endian, as RFC 7748 and the coordinator compute it.
 * Same scalar multiplication as x25519_pub, with the peer's point in place of the base point.
 */
static int x25519_shared(const unsigned char priv[32], const unsigned char peer[32], unsigned char out[32])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point P, R;
    mbedtls_mpi d;
    size_t olen = 0;
    int rc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&P);
    mbedtls_ecp_point_init(&R);
    mbedtls_mpi_init(&d);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) rc = mbedtls_mpi_read_binary_le(&d, priv, 32);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &P, peer, 32);
    if (rc == 0) rc = mbedtls_ecp_mul(&grp, &R, &d, &P, mbedtls_ctr_drbg_random, drbg());
    if (rc == 0) rc = mbedtls_ecp_point_write_binary(&grp, &R, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                                     &olen, out, 32);
    if (rc == 0 && olen != 32) rc = -1;

    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&R);
    mbedtls_ecp_point_free(&P);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

/* HMAC-SHA256 over two parts, either of which may be empty. The md module is in every TLS build. */
static int hmac256(const unsigned char *key, size_t key_len,
                   const unsigned char *a, size_t a_len,
                   const unsigned char *b, size_t b_len, unsigned char out[32])
{
    mbedtls_md_context_t ctx;
    int rc;

    mbedtls_md_init(&ctx);
    rc = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    if (rc == 0) rc = mbedtls_md_hmac_starts(&ctx, key, key_len);
    if (rc == 0 && a_len) rc = mbedtls_md_hmac_update(&ctx, a, a_len);
    if (rc == 0 && b_len) rc = mbedtls_md_hmac_update(&ctx, b, b_len);
    if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    return rc;
}

static void wg_clamp(unsigned char k[32])
{
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
}

/* Crockford-ish base32 without vowels or look-alikes — this gets read off a screen and typed by
 * hand. Must stay byte-identical to FileClaimCodeStore in the .NET agent. */
static void claim_code_new(char out[13])
{
    static const char alphabet[] = "23456789ABCDEFGHJKMNPQRSTVWXYZ";
    unsigned char r[12];
    int i;
    if (mbedtls_ctr_drbg_random(drbg(), r, sizeof r) != 0)
        die("rng failed");
    for (i = 0; i < 12; i++)
        out[i] = alphabet[r[i] % (sizeof alphabet - 1)];
    out[12] = 0;
}

/* ------------------------------------------------------------ subprocesses */

/* Runs argv, optionally capturing stdout. Returns the exit status, or -1 if it could not start. */
static int run(char *const argv[], char *out, size_t out_sz)
{
    int fds[2] = { -1, -1 };
    pid_t pid;
    int status = -1;

    if (out) {
        out[0] = 0;
        if (pipe(fds) != 0) return -1;
    }

    pid = fork();
    if (pid < 0) {
        if (out) { close(fds[0]); close(fds[1]); }
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (out) {
            dup2(fds[1], STDOUT_FILENO);
            close(fds[0]);
            close(fds[1]);
        } else if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
        }
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp(argv[0], argv);
        _exit(127);
    }

    if (out) {
        size_t n = 0;
        ssize_t r;
        close(fds[1]);
        while (n + 1 < out_sz && (r = read(fds[0], out + n, out_sz - n - 1)) > 0)
            n += (size_t)r;
        out[n] = 0;
        close(fds[0]);
    }
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int have(const char *tool)
{
    char *argv[] = { "sh", "-c", NULL, NULL };
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
    argv[2] = cmd;
    return run(argv, NULL, 0) == 0;
}

/* Builds an argv from a printf-style line and runs it. Words are split on spaces — every argument
 * here is a key, an address or an interface name, none of which contain one. */
static int runf(const char *fmt, ...)
{
    char line[1024];
    char *argv[32];
    int argc = 0;
    char *p, *save;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    if (g_cfg.dry_run) {
        note("dry-run: %s", line);
        return 0;
    }

    for (p = strtok_r(line, " ", &save); p && argc < 31; p = strtok_r(NULL, " ", &save))
        argv[argc++] = p;
    argv[argc] = NULL;
    if (argc == 0) return -1;
    return run(argv, NULL, 0);
}

/* ---------------------------------------------------------------- the store */

/*
 * Where the join material lives across reboots. On an OpenIPC camera the rootfs is squashfs and the
 * one thing guaranteed writable is the U-Boot environment, so fw_setenv is the primary backend. A
 * plain file is used when fw_setenv is absent (dev boxes, containers, anything with a real rootfs).
 */
static int store_is_uboot(void)
{
    if (!strcmp(g_cfg.store, "uboot")) return 1;
    if (!strncmp(g_cfg.store, "file:", 5)) return 0;
    return have("fw_setenv");   /* auto */
}

#define STORE_FILE_DEFAULT "/etc/cerberus/agent.env"
#define STORE_FILE_LEGACY  "/etc/streq/agent.env"

static void mkparent(const char *path);

/* The default file moved with the rename; one still in the old place is moved over, once. */
static const char *store_file(void)
{
    static const char *path;

    if (!strncmp(g_cfg.store, "file:", 5)) return g_cfg.store + 5;
    if (path) return path;

    path = STORE_FILE_DEFAULT;
    if (access(STORE_FILE_DEFAULT, F_OK) != 0 && access(STORE_FILE_LEGACY, F_OK) == 0) {
        mkparent(STORE_FILE_DEFAULT);
        if (rename(STORE_FILE_LEGACY, STORE_FILE_DEFAULT) != 0)
            path = STORE_FILE_LEGACY;
    }
    return path;
}

static void mkparent(const char *path)
{
    char buf[512];
    char *slash;
    snprintf(buf, sizeof buf, "%s", path);
    slash = strrchr(buf, '/');
    if (!slash || slash == buf) return;
    *slash = 0;
    if (mkdir(buf, 0700) != 0 && errno != EEXIST)
        warn_("could not create %s: %s", buf, strerror(errno));
}

static int store_get(const char *key, char *out, size_t out_sz)
{
    FILE *f;
    char line[1024];
    size_t klen = strlen(key);

    out[0] = 0;

    if (store_is_uboot()) {
        char *argv[] = { "fw_printenv", "-n", (char *)key, NULL };
        char buf[512];
        if (run(argv, buf, sizeof buf) != 0) return 0;
        buf[strcspn(buf, "\r\n")] = 0;
        if (!buf[0]) return 0;
        snprintf(out, out_sz, "%s", buf);
        return 1;
    }

    f = fopen(store_file(), "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, key, klen) && line[klen] == '=') {
            snprintf(out, out_sz, "%s", line + klen + 1);
            fclose(f);
            return out[0] != 0;
        }
    }
    fclose(f);
    return 0;
}

/* Sets `key`, or removes it when `value` is NULL. */
static void store_put(const char *key, const char *value)
{
    const char *path;
    char tmp[512], line[1024];
    size_t klen = strlen(key);
    FILE *in, *out;

    if (store_is_uboot()) {
        char *argv[] = { "fw_setenv", (char *)key, (char *)value, NULL };   /* no value = unset */
        if (run(argv, NULL, 0) != 0 && value)
            die("fw_setenv %s failed — is /etc/fw_env.config right?", key);
        return;
    }

    /* Rewrite the file with this key replaced. Small and rare enough that a full rewrite beats
     * carrying an in-memory model of it. */
    path = store_file();
    in = fopen(path, "r");
    if (!in && !value) return;
    mkparent(path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    out = fopen(tmp, "w");
    if (!out) die("cannot write %s: %s", tmp, strerror(errno));
    if (in) {
        while (fgets(line, sizeof line, in)) {
            if (!strncmp(line, key, klen) && line[klen] == '=') continue;
            fputs(line, out);
        }
        fclose(in);
    }
    if (value) fprintf(out, "%s=%s\n", key, value);
    fclose(out);
    if (chmod(tmp, 0600) != 0) warn_("chmod %s: %s", tmp, strerror(errno));
    if (rename(tmp, path) != 0) die("cannot replace %s: %s", path, strerror(errno));
}

static void store_set(const char *key, const char *value) { store_put(key, value); }
static void store_unset(const char *key) { store_put(key, NULL); }

/*
 * The agent's keys used to be called streq_*: streq_device_key, streq_node_id, ...
 *
 * What is moved is only what the agent writes itself — identity and join material. Configuration
 * (cerberus_api, cerberus_enrollment_token, ...) is written by people and read by the init script,
 * which looks for both names, so there is nothing to move.
 *
 * The old keys are copied and left where they are, deliberately. An older S99cerberus, or an older
 * binary someone rolls back to, still finds a camera that works; deleting them would buy a tidier
 * `fw_printenv` and a camera that does not come up. Where an old copy could do harm — a node the
 * coordinator has forgotten coming back to life on the next start — both names are removed together
 * (store_forget_enrollment, `clean`).
 *
 * One read of the whole store per start, and no write at all once there is nothing left to move: on
 * a camera every write is a flash erase.
 */
static const char *const g_agent_keys[] = {
    "device_key", "claim_code",     /* first: the identity matters most if this is interrupted */
    "node_id", "overlay_ip", "hub_endpoint", "hub_pubkey", "allowed_ips", "network_subnet", "hub_ip",
    NULL
};

/* `key=value` out of a dump of the whole store; an empty value counts as absent, as in store_get. */
static int dump_get(const char *dump, const char *key, char *out, size_t out_sz)
{
    size_t klen = strlen(key);
    const char *p = dump;

    while (p && *p) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            size_t n = strcspn(p + klen + 1, "\r\n");
            if (n + 1 > out_sz) n = out_sz - 1;
            memcpy(out, p + klen + 1, n);
            out[n] = 0;
            return n > 0;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

static void store_migrate_legacy(void)
{
    static char dump[32768];
    char newk[64], oldk[64], val[512], check[512];
    size_t n = 0;
    int whole, i, moved = 0;

    if (store_is_uboot()) {
        char *argv[] = { "fw_printenv", NULL };
        if (run(argv, dump, sizeof dump) != 0) return;
        n = strlen(dump);
    } else {
        FILE *f = fopen(store_file(), "r");
        if (!f) return;
        n = fread(dump, 1, sizeof dump - 1, f);
        fclose(f);
        dump[n] = 0;
    }
    /* An environment bigger than the buffer is read key by key instead of trusted half-read. */
    whole = n + 1 < sizeof dump;
    if (whole && !strstr(dump, "streq_")) return;

    for (i = 0; g_agent_keys[i]; i++) {
        snprintf(newk, sizeof newk, "cerberus_%s", g_agent_keys[i]);
        snprintf(oldk, sizeof oldk, "streq_%s", g_agent_keys[i]);
        if (whole ? dump_get(dump, newk, val, sizeof val) : store_get(newk, val, sizeof val))
            continue;
        if (!(whole ? dump_get(dump, oldk, val, sizeof val) : store_get(oldk, val, sizeof val)))
            continue;
        store_set(newk, val);
        if (!store_get(newk, check, sizeof check) || strcmp(check, val) != 0)
            die("moving %s to %s did not stick — nothing else is touched until it does", oldk, newk);
        moved++;
    }
    if (moved)
        note("moved %d key(s) from streq_* to cerberus_*; the streq_* copies stay for older init scripts",
             moved);
}

/* Removes both spellings of an agent-written key, so the migration can never bring an old one back. */
static void store_unset_both(const char *suffix)
{
    char key[64];
    snprintf(key, sizeof key, "cerberus_%s", suffix);
    store_unset(key);
    snprintf(key, sizeof key, "streq_%s", suffix);
    store_unset(key);
}

/*
 * Forgets the enrolment, keeping the device's own key and claim code. Called when the coordinator
 * says it has never heard of this node: it was revoked, or rebuilt from under us. Enrolment is
 * skipped whenever a node id is stored, so leaving it in place would turn every later boot into the
 * same dead end — the same reasoning as AgentHost.TryDeleteCache in the .NET agent.
 */
static void store_forget_enrollment(void)
{
    static const char *suffixes[] = {
        "node_id", "overlay_ip", "hub_endpoint", "hub_pubkey", "allowed_ips", "network_subnet",
        "hub_ip", NULL
    };
    int i;

    for (i = 0; suffixes[i]; i++)
        store_unset_both(suffixes[i]);
    warn_("enrolment discarded; restart with a fresh CERBERUS_ENROLLMENT_TOKEN to join again");
}

/* ------------------------------------------------------------------- HTTPS */

struct conn {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    int tls;
};

static int conn_send(struct conn *c, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = c->tls
            ? mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf + sent, len - sent)
            : mbedtls_net_send(&c->net, (const unsigned char *)buf + sent, len - sent);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int conn_recv(struct conn *c, unsigned char *buf, size_t len)
{
    int n;
    do {
        n = c->tls
            ? mbedtls_ssl_read(&c->ssl, buf, len)
            : mbedtls_net_recv(&c->net, buf, len);
    } while (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE);
    return n;
}

static void conn_close(struct conn *c)
{
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_x509_crt_free(&c->ca);
    }
    mbedtls_net_free(&c->net);
}

/*
 * scheme://[user:pass@]host[:port]/path. The coordinator is https; majestic on the camera itself is
 * plain http on loopback, and may want a password — which then rides in the URL, the one place
 * CERBERUS_SNAPSHOT_URL has to put it. `auth` gets "user:pass" base64'd, or stays empty.
 */
static int url_split(const char *url, int *tls, char *host, size_t host_sz,
                     char *port, size_t port_sz, char *path, size_t path_sz,
                     char *auth, size_t auth_sz)
{
    const char *p, *slash, *at;
    char *colon;

    if (!strncmp(url, "https://", 8)) { *tls = 1; p = url + 8; snprintf(port, port_sz, "443"); }
    else if (!strncmp(url, "http://", 7)) { *tls = 0; p = url + 7; snprintf(port, port_sz, "80"); }
    else return -1;

    slash = strchr(p, '/');
    snprintf(path, path_sz, "%s", slash ? slash : "/");

    auth[0] = 0;
    at = memchr(p, '@', slash ? (size_t)(slash - p) : strlen(p));
    if (at) {
        b64((const unsigned char *)p, (size_t)(at - p), auth, auth_sz);
        p = at + 1;
    }
    snprintf(host, host_sz, "%.*s", slash ? (int)(slash - p) : (int)strlen(p), p);

    colon = strrchr(host, ':');
    if (colon && !strchr(host, ']')) {   /* not a bracketed v6 literal */
        snprintf(port, port_sz, "%s", colon + 1);
        *colon = 0;
    }
    return 0;
}

/* Case-insensitive search inside the response head only — never into a JPEG body. */
static const char *head_find(const char *head, size_t head_len, const char *needle)
{
    size_t n = strlen(needle), i, j;
    for (i = 0; i + n <= head_len; i++) {
        for (j = 0; j < n; j++) {
            char a = head[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != b) break;
        }
        if (j == n) return head + i;
    }
    return NULL;
}

/* Undoes Transfer-Encoding: chunked in place. Returns the payload length, or -1 if malformed. */
static long dechunk(unsigned char *buf, size_t len)
{
    size_t in = 0, out = 0;
    while (in < len) {
        char *end;
        unsigned long size = strtoul((const char *)buf + in, &end, 16);
        size_t hdr = (size_t)((unsigned char *)end - buf);
        while (hdr + 1 < len && !(buf[hdr] == '\r' && buf[hdr + 1] == '\n')) hdr++;   /* chunk extensions */
        if (hdr + 1 >= len) return -1;
        in = hdr + 2;
        if (size == 0) return (long)out;
        if (in + size > len) return -1;
        memmove(buf + out, buf + in, size);
        out += size;
        in += size + 2;   /* the CRLF after the chunk */
    }
    return -1;
}

struct http_req {
    const char *method;
    const char *url;
    const char *ctype;           /* NULL when there is no body */
    const unsigned char *body;
    size_t body_len;
    const char *headers;         /* extra header lines, each ending in \r\n; may be NULL */
};

/*
 * One request, one connection, `Connection: close` — so the body is "everything until EOF" and
 * keep-alive bookkeeping is not needed. Binary-safe: the answer may be a JPEG. Chunked answers are
 * de-chunked (majestic's are not, but an HTTP server is allowed to). Returns the body length,
 * NUL-terminated in `resp`, with the status in *status; -1 on a transport failure, -2 when the
 * answer does not fit in resp_sz.
 */
static long http_do(const struct http_req *rq, unsigned char *resp, size_t resp_sz, int *status)
{
    static int tls_said;
    struct conn c;
    char host[256], port[16], path[512], auth[192], head[1536];
    unsigned char buf[4096];
    unsigned char *hdr_end = NULL;
    size_t total = 0, i, head_len;
    long body_len;
    int rc, n, overflow = 0, chunked;

    memset(&c, 0, sizeof c);
    if (url_split(rq->url, &c.tls, host, sizeof host, port, sizeof port, path, sizeof path,
                  auth, sizeof auth) != 0)
        die("unsupported URL: %s", rq->url);

    mbedtls_net_init(&c.net);
    if ((rc = mbedtls_net_connect(&c.net, host, port, MBEDTLS_NET_PROTO_TCP)) != 0) {
        warn_("connect %s:%s failed (-0x%04x)", host, port, (unsigned)-rc);
        return -1;
    }

    if (c.tls) {
        mbedtls_ctr_drbg_context *rng = drbg();   /* before anything TLS: PSA has to be up */
        mbedtls_ssl_init(&c.ssl);
        mbedtls_ssl_config_init(&c.conf);
        mbedtls_x509_crt_init(&c.ca);

        if (mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0)
            die("ssl config failed");

        if (g_cfg.insecure) {
            /* Dev only. Loud, because an enrollment token in the clear is a device someone else
             * can enroll. */
            if (!tls_said) warn_("TLS verification disabled (CERBERUS_INSECURE=1)");
            mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_NONE);
        } else {
            struct stat st;
            int loaded = -1;
            if (stat(g_cfg.ca, &st) == 0)
                loaded = S_ISDIR(st.st_mode)
                    ? mbedtls_x509_crt_parse_path(&c.ca, g_cfg.ca)
                    : mbedtls_x509_crt_parse_file(&c.ca, g_cfg.ca);
            if (loaded < 0)
                die("no usable CA trust store at %s (set CERBERUS_CA)", g_cfg.ca);
            mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&c.conf, &c.ca, NULL);
        }

        mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, rng);
        if (mbedtls_ssl_setup(&c.ssl, &c.conf) != 0) die("ssl setup failed");
        if (mbedtls_ssl_set_hostname(&c.ssl, host) != 0) die("ssl SNI failed");
        mbedtls_ssl_set_bio(&c.ssl, &c.net, mbedtls_net_send, mbedtls_net_recv, NULL);

        while ((rc = mbedtls_ssl_handshake(&c.ssl)) != 0) {
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            warn_("TLS handshake with %s failed (-0x%04x); verify flags 0x%08x",
                  host, (unsigned)-rc, mbedtls_ssl_get_verify_result(&c.ssl));
            conn_close(&c);
            return -1;
        }
        /* Once per process: with motion frames going out every second, a line per request would
         * be most of the log. */
        if (!tls_said++)
            note("TLS ok: %s, %s", mbedtls_ssl_get_version(&c.ssl), mbedtls_ssl_get_ciphersuite(&c.ssl));
    }

    n = snprintf(head, sizeof head,
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: cerberus/" CERBERUS_VERSION "\r\n"
                 "%s%s%s"
                 "%s",
                 rq->method, path, host,
                 auth[0] ? "Authorization: Basic " : "", auth, auth[0] ? "\r\n" : "",
                 rq->headers ? rq->headers : "");
    if (rq->ctype && n > 0 && (size_t)n < sizeof head)
        n += snprintf(head + n, sizeof head - (size_t)n, "Content-Type: %s\r\nContent-Length: %u\r\n",
                      rq->ctype, (unsigned)rq->body_len);
    if (n > 0 && (size_t)n < sizeof head)
        n += snprintf(head + n, sizeof head - (size_t)n, "Connection: close\r\n\r\n");
    if (n <= 0 || (size_t)n >= sizeof head)
        die("request head too long for %s", rq->url);

    if (conn_send(&c, head, (size_t)n) != 0 ||
        (rq->body_len && conn_send(&c, (const char *)rq->body, rq->body_len) != 0)) {
        warn_("sending the request failed");
        conn_close(&c);
        return -1;
    }

    for (;;) {
        n = conn_recv(&c, buf, sizeof buf);
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n <= 0) break;
        if (total + (size_t)n + 1 > resp_sz) { overflow = 1; break; }
        memcpy(resp + total, buf, (size_t)n);
        total += (size_t)n;
    }
    conn_close(&c);
    resp[total] = 0;

    if (total < 12 || strncmp((const char *)resp, "HTTP/1.", 7) != 0) {
        warn_("no HTTP response from %s", host);
        return -1;
    }
    *status = atoi((const char *)resp + 9);

    for (i = 0; i + 3 < total; i++)
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            hdr_end = resp + i;
            break;
        }
    if (!hdr_end) { resp[0] = 0; return overflow ? -2 : 0; }

    /* Look at the head before the body is moved over it. */
    head_len = (size_t)(hdr_end - resp);
    chunked = head_find((const char *)resp, head_len, "transfer-encoding: chunked") != NULL;
    body_len = (long)(total - head_len - 4);
    memmove(resp, hdr_end + 4, (size_t)body_len);
    resp[body_len] = 0;
    if (overflow)
        return -2;   /* what fits is in resp — enough for a JSON caller, never for a frame */
    if (chunked && (body_len = dechunk(resp, (size_t)body_len)) < 0) {
        warn_("malformed chunked answer from %s", host);
        return -1;
    }
    resp[body_len] = 0;
    return body_len;
}

/* The coordinator's JSON calls: text in, text out. Returns 0 when an HTTP answer came back — an
 * answer longer than resp_sz included, cut to fit, as it always was. */
static int http_post_json(const char *url, const char *body, char *resp, size_t resp_sz, int *status)
{
    struct http_req rq = { "POST", url, "application/json", (const unsigned char *)body, strlen(body), NULL };
    return http_do(&rq, (unsigned char *)resp, resp_sz, status) == -1 ? -1 : 0;
}

/* --------------------------------------------------------------- tiny JSON */

/*
 * Pulls one flat string field out of an object. The enroll response has six of them, all ids,
 * addresses or base64 — no escapes, no nesting to walk, no duplicate keys. A real parser would be
 * several times the size of everything else here.
 */
static int json_str(const char *json, const char *key, char *out, size_t out_sz)
{
    char pat[64];
    const char *p, *q;
    size_t n;

    out[0] = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    while (*++p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { }
    if (*p != '"') return 0;
    q = strchr(++p, '"');
    if (!q) return 0;
    n = (size_t)(q - p);
    if (n + 1 > out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

/* ------------------------------------------------------------------ device */

/*
 * The device private key, from the same store the join material lives in.
 *
 * Not from a file on the rootfs, and that is the point. On an OpenIPC camera /etc is squashfs (or an
 * overlay on a partition a firmware update replaces), so a key kept there dies with the next reflash
 * — while `cerberus_node_id` next to it in the U-Boot environment survives. The node would come back
 * with an identity the hub has a peer for and no key to prove it, and enrolment is skipped whenever a
 * node id is stored: not "re-enrollable", *stuck*. Key and node id have to share a lifetime, so they
 * share a store.
 *
 * An older key sitting in CERBERUS_KEYFILE is migrated in on sight, once. It is left on disk rather than
 * deleted: the reflash that motivates all of this will take it anyway, and a delete racing a failed
 * fw_setenv would be the one unrecoverable outcome here.
 */
static void device_key(char priv_b64[64], char pub_b64[64])
{
    unsigned char priv[32], pub[32];
    char stored[128], line[128];
    size_t olen = 0;
    FILE *f;

    if (store_get("cerberus_device_key", stored, sizeof stored) && stored[0]) {
        if (b64_decode(stored, priv, sizeof priv, &olen) != 0 || olen != 32)
            die("cerberus_device_key is not a base64 WireGuard key");
        b64(priv, sizeof priv, priv_b64, 64);   /* re-encode: normalises whatever was stored */
    } else if ((f = fopen(g_cfg.keyfile, "r")) != NULL) {
        int ok = fgets(line, sizeof line, f) != NULL;
        fclose(f);
        line[strcspn(line, "\r\n")] = 0;
        if (!ok || b64_decode(line, priv, sizeof priv, &olen) != 0 || olen != 32)
            die("%s is not a base64 WireGuard key", g_cfg.keyfile);
        b64(priv, sizeof priv, priv_b64, 64);
        store_set("cerberus_device_key", priv_b64);
        note("migrated the device key from %s into the store — it now survives a firmware update",
             g_cfg.keyfile);
    } else {
        if (mbedtls_ctr_drbg_random(drbg(), priv, sizeof priv) != 0) die("rng failed");
        wg_clamp(priv);
        b64(priv, sizeof priv, priv_b64, 64);
        store_set("cerberus_device_key", priv_b64);
        note("generated a device key into the %s store",
             store_is_uboot() ? "u-boot env" : store_file());
    }

    if (x25519_pub(priv, pub) != 0) die("X25519 public key derivation failed");
    b64(pub, sizeof pub, pub_b64, 64);
}

/*
 * `wg set private-key` takes a path, not a key, so the key is materialised for exactly one call:
 * on tmpfs, 0600, unlinked straight after. The copy that persists is the one in the store — which on
 * a camera is the U-Boot environment, and the rootfs is not written at all.
 */
static int key_materialise(char *path, size_t path_sz)
{
    char priv_b64[64], pub_b64[64];
    int fd;

    device_key(priv_b64, pub_b64);
    snprintf(path, path_sz, "%s", cfg_env("KEY_TMP", "/tmp/.cerberus-wg.key"));

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        warn_("cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    if (write(fd, priv_b64, strlen(priv_b64)) < 0 || write(fd, "\n", 1) < 0) {
        warn_("cannot write %s: %s", path, strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return 0;
}

static void claim_code_get(char *code, size_t code_sz)
{
    char fresh[13];
    if (store_get("cerberus_claim_code", code, code_sz) && code[0]) return;
    claim_code_new(fresh);
    store_set("cerberus_claim_code", fresh);
    snprintf(code, code_sz, "%s", fresh);
}

/* ----------------------------------------------------------------- enroll */

/* POST /enroll. Idempotent by construction: a stored nodeId means this
 * camera is already a peer on the hub, and the one-shot token is long gone. */
static int enroll(void)
{
    char priv_b64[64], pub_b64[64], code[16], hash[65];
    char url[512], body[1024], resp[8192];
    char node[128], ip[64], ep[256], hubkey[64], allowed[80], subnet[64], hubip[64];
    int status = 0;

    if (store_get("cerberus_node_id", node, sizeof node)) {
        store_get("cerberus_overlay_ip", ip, sizeof ip);
        note("already enrolled as %s (overlay %s)", node, ip[0] ? ip : "?");
        return 0;
    }

    if (!*g_cfg.token)
        die("not enrolled and no CERBERUS_ENROLLMENT_TOKEN set");

    device_key(priv_b64, pub_b64);
    claim_code_get(code, sizeof code);
    sha256_hex(code, hash);

    snprintf(url, sizeof url, "%s/enroll", g_cfg.api);
    /* agentVersion — U0. Обновлять себя эта камера не умеет (rootfs только для чтения, путь
     * обновления — прошивка), поэтому облако ей не апдейтер, а термометр: пусть владелец видит, что
     * на камере агент позапрошлого года, раньше, чем это выяснится другим способом. */
    if (g_cfg.hostname)
        snprintf(body, sizeof body,
                 "{\"enrollmentToken\":\"%s\",\"devicePublicKey\":\"%s\",\"kind\":\"%s\","
                 "\"hostname\":\"%s\",\"claimCodeHash\":\"%s\",\"agentVersion\":\"%s\"}",
                 g_cfg.token, pub_b64, g_cfg.kind, g_cfg.hostname, hash, CERBERUS_VERSION);
    else
        snprintf(body, sizeof body,
                 "{\"enrollmentToken\":\"%s\",\"devicePublicKey\":\"%s\",\"kind\":\"%s\","
                 "\"claimCodeHash\":\"%s\",\"agentVersion\":\"%s\"}",
                 g_cfg.token, pub_b64, g_cfg.kind, hash, CERBERUS_VERSION);

    note("enrolling at %s as %s", url, pub_b64);
    if (http_post_json(url, body, resp, sizeof resp, &status) != 0)
        return 1;
    if (status != 200) {
        warn_("enroll rejected: HTTP %d %s", status, resp);
        return 1;
    }

    if (!json_str(resp, "nodeId", node, sizeof node) ||
        !json_str(resp, "overlayIp", ip, sizeof ip) ||
        !json_str(resp, "hubEndpoint", ep, sizeof ep) ||
        !json_str(resp, "hubPublicKey", hubkey, sizeof hubkey)) {
        warn_("enroll response missing join material: %s", resp);
        return 1;
    }
    if (!json_str(resp, "allowedIps", allowed, sizeof allowed))
        snprintf(allowed, sizeof allowed, "%s/32", ip);

    /* Подсеть своей сети и адрес хаба. Их может не быть — координатор старше арендаторов; тогда
     * работает CERBERUS_SUBNET, как и раньше. */
    if (!json_str(resp, "networkSubnet", subnet, sizeof subnet)) subnet[0] = 0;
    if (!json_str(resp, "hubOverlayIp", hubip, sizeof hubip)) hubip[0] = 0;

    /* Persist before touching the interface: a camera that got a peer on the hub and then lost the
     * material would re-enroll on the next boot with a token already spent. */
    store_set("cerberus_node_id", node);
    store_set("cerberus_overlay_ip", ip);
    store_set("cerberus_hub_endpoint", ep);
    store_set("cerberus_hub_pubkey", hubkey);
    store_set("cerberus_allowed_ips", allowed);
    if (subnet[0]) store_set("cerberus_network_subnet", subnet);
    if (hubip[0]) store_set("cerberus_hub_ip", hubip);

    note("enrolled as %s, overlay %s/%s via hub %s", node, ip, subnet[0] ? subnet : "?", ep);
    note("claim code: %.4s-%.4s-%.4s — enter it in the panel to bind this camera to an account",
         code, code + 4, code + 8);
    return 0;
}

/* -------------------------------------------------------------- interface */

static int prefix_of(const char *cidr)
{
    const char *slash = strchr(cidr, '/');
    return slash ? atoi(slash + 1) : 16;
}

/*
 * Programs the interface from the stored material. Kernel WireGuard, `wg` and `ip` — all three are
 * already in an OpenIPC image, which is the whole reason this can be a small binary instead of a
 * userspace WireGuard implementation.
 *
 * `flush` is for the rotation path: a new overlay address must replace the old one rather than join
 * it, or the camera keeps answering on an address the coordinator has already handed to somebody else.
 */
/*
 * Подсеть, в которой стоит этот узел: из стора, если координатор её назвал, иначе CERBERUS_SUBNET.
 *
 * With one network per instance these are the same string. With tenants they are not: the node's
 * own /24 is what it may claim on the interface, and CERBERUS_SUBNET is a guess made by whoever wrote
 * the u-boot environment — usually the instance-wide pool, which belongs to everybody.
 */
static const char *node_subnet(void)
{
    static char subnet[64];
    if (store_get("cerberus_network_subnet", subnet, sizeof subnet) && subnet[0])
        return subnet;
    return g_cfg.subnet;
}

/* Overlay-адрес хаба, если координатор его назвал. Пусто — хаб внутри нашей же подсети. */
static const char *hub_overlay_ip(void)
{
    static char hubip[64];
    if (store_get("cerberus_hub_ip", hubip, sizeof hubip) && hubip[0])
        return hubip;
    return "";
}

static int link_apply(int flush)
{
    char ip[64], ep[256], hubkey[64], keypath[256];
    const char *subnet, *hubip;
    int rc;

    if (!store_get("cerberus_overlay_ip", ip, sizeof ip) ||
        !store_get("cerberus_hub_endpoint", ep, sizeof ep) ||
        !store_get("cerberus_hub_pubkey", hubkey, sizeof hubkey)) {
        warn_("no stored join material — run `cerberus enroll` first");
        return 1;
    }

    if (key_materialise(keypath, sizeof keypath) != 0)
        return 1;

    subnet = node_subnet();
    hubip = hub_overlay_ip();

    runf("ip link add dev %s type wireguard", g_cfg.iface);   /* already there → non-zero, fine */

    /* Хаб живёт вне подсети арендатора, поэтому он вторым allowed-ip — и ниже вторым маршрутом.
     * allowed-ips это криптомаршрутизация WireGuard, таблицы ядра она не трогает вовсе. */
    rc = *hubip
        ? runf("wg set %s private-key %s peer %s endpoint %s allowed-ips %s,%s/32 persistent-keepalive 25",
               g_cfg.iface, keypath, hubkey, ep, subnet, hubip)
        : runf("wg set %s private-key %s peer %s endpoint %s allowed-ips %s persistent-keepalive 25",
               g_cfg.iface, keypath, hubkey, ep, subnet);
    unlink(keypath);
    if (rc != 0) {
        warn_("wg set failed on %s", g_cfg.iface);
        return 1;
    }

    if (flush)
        runf("ip address flush dev %s", g_cfg.iface);
    if (runf("ip address replace %s/%d dev %s", ip, prefix_of(subnet), g_cfg.iface) != 0 ||
        runf("ip link set up dev %s", g_cfg.iface) != 0) {
        warn_("bringing %s up failed", g_cfg.iface);
        return 1;
    }
    /* Без этого маршрута пакет к хабу уходит через обычный шлюз: туннель поднят, а связи нет. */
    if (*hubip)
        runf("ip route replace %s/32 dev %s", hubip, g_cfg.iface);

    note("overlay up: %s in %s on %s via hub %s", ip, subnet, g_cfg.iface, ep);
    return 0;
}

static int link_up(void)
{
    return link_apply(0);
}

/*
 * Tears the interface down and rebuilds it. The heavy hammer, for when the tunnel is up on paper and
 * dead in fact: `wg set` cannot fix an interface whose address or route was taken over by something
 * else, and nobody is going to walk up to the camera and find out which it was.
 */
static int link_reset(void)
{
    warn_("rebuilding %s from scratch", g_cfg.iface);
    runf("ip link del dev %s", g_cfg.iface);
    return link_apply(1);
}

/* Seconds since the last completed handshake with the hub, or -1 when there has never been one. */
static long handshake_age(void)
{
    char *argv[] = { "wg", "show", NULL, "latest-handshakes", NULL };
    char out[1024];
    const char *tab;
    long ts;

    argv[2] = (char *)g_cfg.iface;
    if (run(argv, out, sizeof out) != 0) return -1;

    tab = strchr(out, '\t');
    if (!tab) return -1;
    ts = strtol(tab + 1, NULL, 10);
    if (ts <= 0) return -1;
    return (long)time(NULL) - ts;
}

/* ---------------------------------------------------------- in-tunnel probe */

/*
 * Кого пинговать внутри туннеля. Хаб: единственный пир камеры и первый адрес подсети (10.80.0.0/16 →
 * 10.80.0.1, ровно так его поднимает хаб). Материал вступления его не называет —
 * узлу он больше ни для чего не нужен — поэтому адрес выводится из подсети, а CERBERUS_PING_TARGET
 * перекрывает вывод там, где хаб стоит не на первом адресе.
 */
static const char *ping_target(void)
{
    static char target[64];
    unsigned a = 0, b = 0, c = 0, d = 0;
    int prefix = 0;
    unsigned long net;

    if (*g_cfg.ping_target) return g_cfg.ping_target;
    if (target[0]) return target;

    /* Координатор назвал адрес хаба — пингуем его, а не вывод из подсети: у арендатора хаб вообще
     * не в его диапазоне. */
    if (*hub_overlay_ip()) {
        snprintf(target, sizeof target, "%s", hub_overlay_ip());
        return target;
    }

    if (sscanf(g_cfg.subnet, "%u.%u.%u.%u/%d", &a, &b, &c, &d, &prefix) < 4)
        return "";
    if (prefix <= 0 || prefix > 30) prefix = 16;

    net = ((unsigned long)a << 24) | ((unsigned long)b << 16) | ((unsigned long)c << 8) | d;
    net &= (0xFFFFFFFFUL << (32 - prefix)) & 0xFFFFFFFFUL;
    net += 1;
    snprintf(target, sizeof target, "%lu.%lu.%lu.%lu",
             (net >> 24) & 0xFF, (net >> 16) & 0xFF, (net >> 8) & 0xFF, net & 0xFF);
    return target;
}

/*
 * Одна ICMP-проба через туннель: 1 — дошло, 0 — нет, -1 — пробовать нечем.
 *
 * Why this sits next to the handshake check rather than replacing it: they fail for different reasons
 * and neither implies the other. WireGuard re-handshakes about every two minutes and will show a
 * perfectly fresh one while nothing at all traverses the tunnel — a route that got taken, an
 * interface someone else reused, a hub that completes handshakes and forwards nothing. The handshake
 * says the peer is there; only a packet that comes back says the tunnel works.
 *
 * Shelling out to busybox ping instead of opening a raw socket: the firmware already ships one, a raw
 * socket is another privileged path to get wrong, and this runs once a minute.
 */
static int ping_ok(void)
{
    char *argv[] = { "ping", "-c", "1", "-W", NULL, NULL, NULL };
    char timeout[16];
    const char *target = ping_target();
    int rc;

    if (!*target) return -1;

    snprintf(timeout, sizeof timeout, "%d", g_cfg.ping_timeout);
    argv[4] = timeout;
    argv[5] = (char *)target;

    rc = run(argv, NULL, 0);
    if (rc == 127 || rc < 0) return -1;   /* no ping binary — the probe is simply unavailable */
    return rc == 0;
}

/* ------------------------------------------------------------------ events */

/*
 * Motion, end to end on the camera's side.
 *
 * majestic runs a hook script when it sees motion (its own detector — free, always on once enabled
 * in its settings). Our hook does nothing but wake us with SIGUSR1: signing, TLS, retries and the
 * coordinator's address all live here, and a second HTTP client in a shell script would be a second
 * set of bugs. One episode is motion → quiet for CERBERUS_MOTION_QUIET seconds → over; it goes out
 * twice, `started` at once and `ended` with its length, however many times the hook fired between.
 *
 * The answer to `started` says how many frames the cloud wants. Zero unless the owner switched
 * object detection on for this camera; then a frame a second, taken by majestic itself
 * (CERBERUS_SNAPSHOT_URL), until the cloud says it has seen enough. So the cloud gets the picture
 * from the moment of motion, without pulling the stream and without the RTSP password.
 *
 * Every request is signed: the heartbeat is anonymous, an event is not — it ends up as a message
 * with a picture in somebody's Telegram. The key is derived, not stored:
 *   shared = X25519(device key, coordinator's eventKey from the directive)
 *   k      = HKDF-SHA256(ikm = shared, salt = nodeId, info = "streq-events-v1")
 *   sig    = base64(HMAC-SHA256(k, ts "\n" nodeId "\n" scope "\n" body))
 * and lives exactly as long as the device key does.
 */

static volatile sig_atomic_t g_motion;
static void on_motion(int sig) { (void)sig; g_motion = 1; }

static char g_node[128];
static char g_event_pub[64];
static unsigned char g_event_k[32];
static int g_event_ready;

/* Flat JSON number field; `fallback` when absent. Same contract as json_str. */
static long json_int(const char *json, const char *key, long fallback)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':'))) return fallback;
    while (*++p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { }
    return (*p == '-' || (*p >= '0' && *p <= '9')) ? strtol(p, NULL, 10) : fallback;
}

static int json_true(const char *json, const char *key)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':'))) return 0;
    while (*++p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { }
    return !strncmp(p, "true", 4);
}

/* The directive hands the coordinator's eventKey on every beat; the key is derived once per value. */
static void event_key_update(const char *coord_pub_b64)
{
    static const char info[] = "streq-events-v1";
    static const unsigned char one = 1;
    unsigned char coord[48], priv[48], shared[32], prk[32];
    char priv_b64[64], pub_b64[64];
    size_t clen = 0, plen = 0;

    if (!g_node[0] || (g_event_ready && !strcmp(coord_pub_b64, g_event_pub)))
        return;
    if (b64_decode(coord_pub_b64, coord, sizeof coord, &clen) != 0 || clen != 32) {
        warn_("the coordinator's eventKey is not a 32-byte key — motion events stay off");
        return;
    }

    device_key(priv_b64, pub_b64);
    if (b64_decode(priv_b64, priv, sizeof priv, &plen) != 0 || plen != 32 ||
        x25519_shared(priv, coord, shared) != 0 ||
        /* HKDF (RFC 5869) with one output block: PRK = HMAC(salt, IKM), OKM = HMAC(PRK, info | 0x01). */
        hmac256((const unsigned char *)g_node, strlen(g_node), shared, sizeof shared, NULL, 0, prk) != 0 ||
        hmac256(prk, sizeof prk, (const unsigned char *)info, strlen(info), &one, 1, g_event_k) != 0) {
        warn_("could not derive the event signing key — motion events stay off");
        g_event_ready = 0;
    } else {
        snprintf(g_event_pub, sizeof g_event_pub, "%s", coord_pub_b64);
        if (!g_event_ready) note("motion events: the coordinator takes them from this camera");
        g_event_ready = 1;
    }
    memset(priv, 0, sizeof priv);
    memset(shared, 0, sizeof shared);
    memset(prk, 0, sizeof prk);
    memset(priv_b64, 0, sizeof priv_b64);
}

/* POST <api><path>, signed for `scope`. Returns the HTTP status, or -1 when there was no answer. */
static int event_post(const char *path, const char *scope, const char *ctype,
                      const unsigned char *body, size_t len, char *resp, size_t resp_sz)
{
    char url[512], ts[24], signed_head[256], sig[64], hdr[256];
    unsigned char mac[32];
    struct http_req rq;
    int n, status = 0;

    snprintf(ts, sizeof ts, "%ld", (long)time(NULL));
    n = snprintf(signed_head, sizeof signed_head, "%s\n%s\n%s\n", ts, g_node, scope);
    if (n <= 0 || (size_t)n >= sizeof signed_head ||
        hmac256(g_event_k, sizeof g_event_k, (const unsigned char *)signed_head, (size_t)n, body, len, mac) != 0)
        return -1;
    b64(mac, sizeof mac, sig, sizeof sig);
    snprintf(hdr, sizeof hdr, "X-Streq-Ts: %s\r\nX-Streq-Sig: %s\r\n", ts, sig);
    snprintf(url, sizeof url, "%s%s", g_cfg.api, path);

    rq.method = "POST";
    rq.url = url;
    rq.ctype = ctype;
    rq.body = body;
    rq.body_len = len;
    rq.headers = hdr;
    return http_do(&rq, (unsigned char *)resp, resp_sz, &status) == -1 ? -1 : status;
}

static unsigned char *g_frame;

/* One JPEG from majestic into g_frame. Returns its length, or -1. */
static long snapshot_take(void)
{
    static int complaints;
    struct http_req rq = { "GET", NULL, NULL, NULL, 0, NULL };
    int status = 0;
    long n;

    if (!g_frame && !(g_frame = malloc((size_t)g_cfg.snapshot_max + 1)))
        return -1;
    rq.url = g_cfg.snapshot_url;
    n = http_do(&rq, g_frame, (size_t)g_cfg.snapshot_max + 1, &status);
    if (n == -1)
        return -1;
    if (status == 401 || status == 403) {
        if (complaints++ < 3)
            warn_("majestic wants a password for %s — put it in the address: "
                  "fw_setenv cerberus_snapshot_url 'http://user:pass@127.0.0.1/image.jpg?width=640&height=360'",
                  g_cfg.snapshot_url);
        return -1;
    }
    if (n == -2) {
        if (complaints++ < 3)
            warn_("the snapshot is larger than %d bytes — ask majestic for a smaller one (width=640)", g_cfg.snapshot_max);
        return -1;
    }
    if (status != 200 || n < 4 || g_frame[0] != 0xFF || g_frame[1] != 0xD8) {
        if (complaints++ < 3)
            warn_("the snapshot is not a JPEG (HTTP %d, %ld bytes) — check CERBERUS_SNAPSHOT_URL", status, n);
        return -1;
    }
    return n;
}

static struct {
    int active;
    char id[32];
    time_t started, last_motion, next_frame;
    int frames_left, interval, seq, misses;
} g_ep;

static void episode_post(const char *phase, long duration, char *resp, size_t resp_sz, int *status)
{
    char path[256], body[192];

    if (duration >= 0)
        snprintf(body, sizeof body, "{\"eventId\":\"%s\",\"kind\":\"motion\",\"phase\":\"%s\",\"durationSec\":%ld}",
                 g_ep.id, phase, duration);
    else
        snprintf(body, sizeof body, "{\"eventId\":\"%s\",\"kind\":\"motion\",\"phase\":\"%s\"}", g_ep.id, phase);
    snprintf(path, sizeof path, "/nodes/%s/events", g_node);
    *status = event_post(path, "event", "application/json", (const unsigned char *)body, strlen(body),
                         resp, resp_sz);
}

static void episode_start(time_t now)
{
    static int said_clock, said_refused;
    unsigned char r[12];
    char resp[1024];
    int status, i;

    /* Signatures carry the time; a camera still at 1970 would be refused on every episode. */
    if (now < 1600000000) {
        if (!said_clock++) warn_("motion: the clock is not set (no NTP yet?) — events wait for it");
        return;
    }

    memset(&g_ep, 0, sizeof g_ep);
    if (mbedtls_ctr_drbg_random(drbg(), r, sizeof r) != 0) return;
    g_ep.id[0] = 'm';
    for (i = 0; i < 12; i++)
        sprintf(g_ep.id + 1 + i * 2, "%02x", r[i]);
    g_ep.active = 1;
    g_ep.started = g_ep.last_motion = g_ep.next_frame = now;
    g_ep.interval = 1;

    episode_post("started", -1, resp, sizeof resp, &status);
    if (status == 200) {
        g_ep.frames_left = (int)json_int(resp, "frames", 0);
        g_ep.interval = (int)json_int(resp, "intervalSeconds", 1);
        if (g_ep.interval < 1) g_ep.interval = 1;
        note("motion: episode %s%s", g_ep.id,
             g_ep.frames_left > 0 ? ", the cloud looks for people and cars in it" : "");
    } else if (status == 401) {
        if (!said_refused++)
            warn_("motion: the coordinator refused the signature — check the camera's clock");
    } else if (status == 429) {
        warn_("motion: too many episodes this hour — narrow majestic's motion zone or sensitivity");
    } else {
        warn_("motion: the event was not taken (HTTP %d)", status);
    }
}

static void episode_frame(time_t now)
{
    char path[256], scope[96], resp[1024];
    long n;
    int status;

    g_ep.next_frame = now + g_ep.interval;
    if ((n = snapshot_take()) < 0) {
        if (++g_ep.misses >= 3) g_ep.frames_left = 0;
        return;
    }

    snprintf(path, sizeof path, "/nodes/%s/events/%s/frames/%d", g_node, g_ep.id, g_ep.seq);
    snprintf(scope, sizeof scope, "frame/%s/%d", g_ep.id, g_ep.seq);
    status = event_post(path, scope, "image/jpeg", g_frame, (size_t)n, resp, sizeof resp);
    g_ep.seq++;
    g_ep.frames_left--;
    if (status != 200) {
        warn_("motion: frame %d was not taken (HTTP %d)", g_ep.seq - 1, status);
        g_ep.frames_left = 0;
    } else if (!json_true(resp, "more")) {
        g_ep.frames_left = 0;
        if (strstr(resp, "\"labels\":[\""))
            note("motion: the cloud has seen enough: %s", resp);
    }
}

static void episode_end(time_t now)
{
    char resp[512];
    int status;
    long duration = (long)(g_ep.last_motion - g_ep.started);

    episode_post("ended", duration, resp, sizeof resp, &status);
    if (status != 200)
        warn_("motion: the end of episode %s was not taken (HTTP %d)", g_ep.id, status);
    note("motion: episode %s over after %lds, %d frame(s) sent", g_ep.id, duration, g_ep.seq);
    g_ep.active = 0;
    (void)now;
}

/* One step of the motion machinery; called every loop turn, which is every second in an episode. */
static void events_tick(time_t now)
{
    static int said_no_key;

    if (g_motion) {
        g_motion = 0;
        if (g_ep.active)
            g_ep.last_motion = now;
        else if (g_event_ready)
            episode_start(now);
        else if (!said_no_key++)
            note("motion seen, but the coordinator has not handed this camera an event key (older cloud?)");
    }
    if (!g_ep.active)
        return;
    if (g_ep.frames_left > 0 && now >= g_ep.next_frame)
        episode_frame(now);
    if (now - g_ep.last_motion >= g_cfg.motion_quiet || now - g_ep.started >= g_cfg.motion_max)
        episode_end(time(NULL));
}

/*
 * The hook majestic runs. Ours only wakes the agent — and checks that the pid in the pidfile is
 * still us before signalling it, because a pid outlives the process it named, and SIGUSR1 kills
 * whatever does not expect it. Someone else's hook is not overwritten: it is kept next to ours as
 * <hook>.orig and still called, with the same arguments.
 */
#define HOOK_MARK "CERBERUS_HOOK=1"

static int file_has(const char *path, const char *needle)
{
    char buf[2048];
    size_t n;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

static void motion_hook_install(void)
{
    char orig[320], tmp[320], body[1024];
    struct stat st;
    FILE *f;

    snprintf(orig, sizeof orig, "%s.orig", g_cfg.motion_hook);
    snprintf(tmp, sizeof tmp, "%s.cerberus-new", g_cfg.motion_hook);
    snprintf(body, sizeof body,
             "#!/bin/sh\n"
             HOOK_MARK "\n"
             "[ -x %s ] && %s \"$@\" &\n"
             "p=$(cat %s 2>/dev/null)\n"
             "[ -n \"$p\" ] && grep -q cerberus /proc/\"$p\"/cmdline 2>/dev/null && kill -USR1 \"$p\"\n"
             "exit 0\n",
             orig, orig, g_cfg.pidfile);

    if (stat(g_cfg.motion_hook, &st) == 0) {
        if (file_has(g_cfg.motion_hook, body))
            return;   /* ours, current */
        if (!file_has(g_cfg.motion_hook, HOOK_MARK)) {
            if (stat(orig, &st) == 0) {
                warn_("%s is not ours and %s exists already — leaving both alone; motion events are off",
                      g_cfg.motion_hook, orig);
                return;
            }
            if (rename(g_cfg.motion_hook, orig) != 0) {
                warn_("cannot keep %s aside: %s — motion events are off", g_cfg.motion_hook, strerror(errno));
                return;
            }
            note("kept the existing %s as %s; ours calls it", g_cfg.motion_hook, orig);
        }
    }

    mkparent(g_cfg.motion_hook);
    if (!(f = fopen(tmp, "w"))) {
        warn_("cannot write %s: %s — motion events are off", tmp, strerror(errno));
        return;
    }
    fputs(body, f);
    fclose(f);
    chmod(tmp, 0755);
    if (rename(tmp, g_cfg.motion_hook) != 0) {
        warn_("cannot install %s: %s — motion events are off", g_cfg.motion_hook, strerror(errno));
        unlink(tmp);
        return;
    }
    note("motion hook installed at %s (majestic runs it when motion detection is on)", g_cfg.motion_hook);
}

/* `clean`: our hook goes, and whatever it replaced comes back. */
static void motion_hook_remove(void)
{
    char orig[320];
    struct stat st;

    if (!file_has(g_cfg.motion_hook, HOOK_MARK))
        return;
    snprintf(orig, sizeof orig, "%s.orig", g_cfg.motion_hook);
    if (stat(orig, &st) == 0 ? rename(orig, g_cfg.motion_hook) : unlink(g_cfg.motion_hook))
        warn_("cannot remove the motion hook %s: %s", g_cfg.motion_hook, strerror(errno));
}

static void pidfile_write(void)
{
    FILE *f;
    mkparent(g_cfg.pidfile);
    if ((f = fopen(g_cfg.pidfile, "w")) != NULL) {
        fprintf(f, "%ld\n", (long)getpid());
        fclose(f);
    }
}

/* `cerberus motion`: what the hook does, by hand — the way to test without walking past the camera. */
static int cmd_motion(void)
{
    char buf[32];
    long pid = 0;
    FILE *f = fopen(g_cfg.pidfile, "r");

    if (f) {
        if (fgets(buf, sizeof buf, f)) pid = atol(buf);
        fclose(f);
    }
    if (pid <= 0 || kill((pid_t)pid, SIGUSR1) != 0) {
        fprintf(stderr, "cerberus: no running agent in %s\n", g_cfg.pidfile);
        return 1;
    }
    printf("motion signalled to %ld\n", pid);
    return 0;
}

/* `cerberus snapshot`: is majestic's frame reachable as configured? Saves it for a look. */
static int cmd_snapshot(void)
{
    const char *out = "/tmp/cerberus-snapshot.jpg";
    long n = snapshot_take();
    FILE *f;

    if (n < 0) {
        fprintf(stderr, "cerberus: no snapshot from %s\n", g_cfg.snapshot_url);
        return 1;
    }
    if ((f = fopen(out, "wb")) != NULL) {
        fwrite(g_frame, 1, (size_t)n, f);
        fclose(f);
    }
    printf("snapshot: %ld bytes -> %s\n", n, out);
    return 0;
}

/* ------------------------------------------------------------- directives */

static char **g_argv;

/* Перезапуск самого агента: re-exec, потому что супервизора на камере может и не оказаться. */
static void self_restart(void)
{
    note("restarting");
    fflush(NULL);
    execv("/proc/self/exe", g_argv);
    execvp(g_argv[0], g_argv);
    /* Both failed: exit non-zero and let whatever started us decide. */
    die("restart failed: %s", strerror(errno));
}

static void do_reboot(void)
{
    char *argv[] = { "reboot", NULL };
    char *fallback[] = { "/sbin/reboot", NULL };

    warn_("rebooting on the coordinator's command");
    sync();
    if (run(argv, NULL, 0) != 0 && run(fallback, NULL, 0) != 0)
        warn_("reboot failed — no usable reboot binary");
}

/*
 * Применяет NodeDirective из ответа на heartbeat.
 *
 * The join material arrives whole on every beat, never as a delta, so this compares it with what is
 * stored and acts only on a difference — that is what makes a hub that moved or re-keyed survivable
 * without visiting the camera. Persist first, reprogram second: a camera that reprogrammed and then
 * lost the material would come back on the next boot pointing at a hub that no longer exists.
 */
static void apply_directive(const char *json)
{
    char ep[256], hubkey[64], ip[64], allowed[80], fresh[64];
    char cur[256], command[32];
    int rotated = 0;

    if (json_str(json, "hubEndpoint", ep, sizeof ep) &&
        (!store_get("cerberus_hub_endpoint", cur, sizeof cur) || strcmp(cur, ep) != 0)) {
        note("hub endpoint rotated: %s -> %s", cur[0] ? cur : "(none)", ep);
        store_set("cerberus_hub_endpoint", ep);
        rotated = 1;
    }
    if (json_str(json, "hubPublicKey", hubkey, sizeof hubkey) &&
        (!store_get("cerberus_hub_pubkey", cur, sizeof cur) || strcmp(cur, hubkey) != 0)) {
        note("hub public key rotated");
        store_set("cerberus_hub_pubkey", hubkey);
        rotated = 1;
    }
    if (json_str(json, "overlayIp", ip, sizeof ip) &&
        (!store_get("cerberus_overlay_ip", cur, sizeof cur) || strcmp(cur, ip) != 0)) {
        note("overlay address rotated: %s -> %s", cur[0] ? cur : "(none)", ip);
        store_set("cerberus_overlay_ip", ip);
        rotated = 1;
    }
    if (json_str(json, "allowedIps", allowed, sizeof allowed))
        store_set("cerberus_allowed_ips", allowed);
    if (json_str(json, "networkSubnet", fresh, sizeof fresh) && fresh[0] &&
        (!store_get("cerberus_network_subnet", cur, sizeof cur) || strcmp(cur, fresh) != 0)) {
        note("network subnet rotated: %s -> %s", cur[0] ? cur : "(none)", fresh);
        store_set("cerberus_network_subnet", fresh);
        rotated = 1;
    }
    if (json_str(json, "hubOverlayIp", fresh, sizeof fresh) && fresh[0] &&
        (!store_get("cerberus_hub_ip", cur, sizeof cur) || strcmp(cur, fresh) != 0)) {
        note("hub overlay address rotated: %s -> %s", cur[0] ? cur : "(none)", fresh);
        store_set("cerberus_hub_ip", fresh);
        rotated = 1;
    }

    if (rotated && link_apply(1) != 0)
        warn_("could not apply the rotated join material; the stale-handshake path will retry");

    if (g_cfg.events && json_str(json, "eventKey", fresh, sizeof fresh) && fresh[0])
        event_key_update(fresh);

    if (!json_str(json, "command", command, sizeof command))
        return;
    if (!strcmp(command, "Reboot"))
        do_reboot();
    else if (!strcmp(command, "Restart"))
        self_restart();
    else if (strcmp(command, "None") != 0)
        warn_("unknown command '%s' from the coordinator — ignoring", command);
}

/* ------------------------------------------------------------------ watch */

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/*
 * Ставятся до энролла, а не в watch(): между стартом и watch() может пройти сколько угодно времени,
 * если координатор недоступен, и всё это время камера обязана слышать SIGTERM — иначе перезагрузка
 * ждёт полный таймаут убийцы процессов.
 */
static void catch_signals(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    /* The motion hook's wake-up. SIGUSR1 kills by default, so this is set before anything slow. */
    signal(SIGUSR1, on_motion);
}

/*
 * The whole "monitor that the tunnel is alive" job, on three timers.
 *
 * WireGuard is stateless enough that there is no connection to repair — but the hub's public address
 * can move (DHCP, a redeployed VPS), and `wg` resolved that name exactly once, at set time. Re-setting
 * the peer re-resolves it, which is the cheapest recovery a camera can perform on its own.
 *
 * Past that the escalation is laddered on purpose, because each rung costs more than the one below:
 * re-poke the peer (nothing is interrupted) → rebuild the interface (a second of downtime) → restart
 * the agent (everything re-derived from the store). A camera nobody can reach has to be able to work
 * through all three by itself; a camera that reboots itself over one lost packet is worse than one
 * that stays down.
 */
static int watch(void)
{
    char ep[256], hubkey[64], node[128], beat_url[512], resp[4096];
    int misses = 0, beat_fails = 0, ping_misses = 0, ping_available = 1, status;
    time_t next_check = 0, next_beat = 0, next_ping = 0, now;
    int tick;

    catch_signals();   /* повторно — безвредно; watch() бывает и точкой входа отладки */

    if (!store_get("cerberus_hub_endpoint", ep, sizeof ep) ||
        !store_get("cerberus_hub_pubkey", hubkey, sizeof hubkey))
        return 1;
    store_get("cerberus_node_id", node, sizeof node);
    snprintf(beat_url, sizeof beat_url, "%s/nodes/%s/heartbeat", g_cfg.api, node);
    snprintf(g_node, sizeof g_node, "%s", node);

    if (g_cfg.events) {
        pidfile_write();
        motion_hook_install();
    }

    tick = g_cfg.watch_interval;
    if (g_cfg.heartbeat > 0 && g_cfg.heartbeat < tick) tick = g_cfg.heartbeat;
    if (g_cfg.ping_interval > 0 && g_cfg.ping_interval < tick) tick = g_cfg.ping_interval;

    note("watching %s (handshake every %ds, stale after %ds, heartbeat %ds, ping %s every %ds)",
         g_cfg.iface, g_cfg.watch_interval, g_cfg.stale_after, g_cfg.heartbeat,
         g_cfg.ping_interval > 0 ? ping_target() : "off", g_cfg.ping_interval);

    while (!g_stop) {
        now = time(NULL);

        if (now >= next_check) {
            long age = handshake_age();
            if (age >= 0 && age <= g_cfg.stale_after) {
                misses = 0;
            } else {
                misses++;
                warn_("no handshake for %lds (miss %d) — re-resolving %s", age, misses, ep);
                /* Re-read: a directive may have rotated the hub since this loop started. */
                store_get("cerberus_hub_endpoint", ep, sizeof ep);
                store_get("cerberus_hub_pubkey", hubkey, sizeof hubkey);
                if (*hub_overlay_ip())
                    runf("wg set %s peer %s endpoint %s allowed-ips %s,%s/32 persistent-keepalive 25",
                         g_cfg.iface, hubkey, ep, node_subnet(), hub_overlay_ip());
                else
                    runf("wg set %s peer %s endpoint %s allowed-ips %s persistent-keepalive 25",
                         g_cfg.iface, hubkey, ep, node_subnet());
            }
            next_check = now + g_cfg.watch_interval;
        }

        /* Does anything actually traverse the tunnel? Escalates only on a run of failures: one lost
         * ICMP packet is weather, three in a row is a dead path. */
        if (g_cfg.ping_interval > 0 && ping_available && now >= next_ping) {
            int probe = ping_ok();
            if (probe < 0) {
                ping_available = 0;
                warn_("no usable `ping` — the in-tunnel probe is off, the handshake watch stands");
            } else if (probe) {
                if (ping_misses > 0) note("the tunnel answers again (%s)", ping_target());
                ping_misses = 0;
            } else {
                ping_misses++;
                warn_("no reply from %s inside the tunnel (miss %d)", ping_target(), ping_misses);
                if (ping_misses >= g_cfg.ping_fails * 2) {
                    warn_("the tunnel stayed dead through a rebuild — restarting the agent");
                    self_restart();
                } else if (ping_misses == g_cfg.ping_fails) {
                    link_reset();
                }
            }
            next_ping = time(NULL) + g_cfg.ping_interval;
        }

        /* Liveness — and the only back-channel a thin node has, so the answer is read, not discarded.
         * The node is perfectly usable without it (relay tokens never consult lastSeen), so a
         * coordinator that is unreachable is worth one warning, not a retry storm. */
        if (g_cfg.heartbeat > 0 && node[0] && now >= next_beat) {
            status = 0;
            if (http_post_json(beat_url, "{\"agentVersion\":\"" CERBERUS_VERSION "\"}",
                               resp, sizeof resp, &status) != 0 || status >= 500) {
                if (beat_fails++ == 0)
                    warn_("heartbeat to %s failed (HTTP %d) — the overlay is unaffected", beat_url, status);
            } else {
                if (beat_fails > 0) note("heartbeat recovered after %d failure(s)", beat_fails);
                beat_fails = 0;
                if (status == 404) {
                    /* The coordinator does not know this node any more. */
                    store_forget_enrollment();
                    g_stop = 1;
                } else if (status == 200) {
                    apply_directive(resp);
                }
            }
            next_beat = time(NULL) + g_cfg.heartbeat;
        }

        if (g_cfg.events)
            events_tick(time(NULL));

        /* A second at a time while an episode runs: frames go out every second, and the hook's
         * signal cuts any sleep short anyway. */
        if (!g_stop && !g_motion) sleep(g_ep.active ? 1u : (unsigned)tick);
    }
    if (g_ep.active)
        episode_end(time(NULL));
    note("stopping");
    return 0;
}

/*
 * Энролл с повторами. Агент не имеет права умереть от того, что координатор недоступен в секунду
 * его старта.
 *
 * BusyBox init runs S99cerberus once and then supervises nothing at all, so an exit here is the
 * camera gone until somebody power-cycles it — and the most ordinary boot in the world has the
 * network, DNS or NTP arriving a few seconds after we do. The .NET agent gets this for free from
 * systemd's Restart=always; on a camera there is no systemd to get it from, so the waiting has to
 * live here.
 *
 * Отступает до потолка, а не долбится: недоступный координатор недоступен минутами, а не
 * миллисекундами, и камера на мобильном тарифе платит за каждую попытку. Ошибки конфигурации при
 * этом по-прежнему убивают сразу — enroll() зовёт для них die(), и никакое количество повторов не
 * выдумает токен, которого не прописали.
 */
static int enroll_with_retry(void)
{
    int delay = g_cfg.retry_min;
    unsigned slept;

    while (!g_stop) {
        if (enroll() == 0)
            return 0;
        if (g_stop)
            break;

        warn_("enrolment did not go through; retrying in %ds", delay);
        for (slept = 0; slept < (unsigned)delay && !g_stop; slept++)
            sleep(1);

        delay *= 2;
        if (delay > g_cfg.retry_max)
            delay = g_cfg.retry_max;
    }
    return 1;
}

/* ------------------------------------------------------------------- main */

static int cmd_status(void)
{
    char v[512];
    long age;
    printf("store          %s\n", store_is_uboot() ? "u-boot env (fw_setenv)" : store_file());
    printf("node           %s\n", store_get("cerberus_node_id", v, sizeof v) ? v : "(not enrolled)");
    printf("device key     %s\n", store_get("cerberus_device_key", v, sizeof v) ? "in the store" : "-");
    printf("overlay ip     %s\n", store_get("cerberus_overlay_ip", v, sizeof v) ? v : "-");
    printf("hub endpoint   %s\n", store_get("cerberus_hub_endpoint", v, sizeof v) ? v : "-");
    printf("hub public key %s\n", store_get("cerberus_hub_pubkey", v, sizeof v) ? v : "-");
    printf("network subnet %s%s\n", node_subnet(),
           store_get("cerberus_network_subnet", v, sizeof v) ? "" : " (from CERBERUS_SUBNET)");
    printf("hub overlay ip %s\n", *hub_overlay_ip() ? hub_overlay_ip() : "- (inside its own subnet)");
    printf("claim code     %s\n", store_get("cerberus_claim_code", v, sizeof v) ? v : "-");
    age = handshake_age();
    if (age < 0) printf("handshake      never\n");
    else printf("handshake      %lds ago\n", age);
    if (g_cfg.ping_interval > 0) {
        int probe = ping_ok();
        printf("tunnel ping    %s (%s)\n",
               probe < 0 ? "unavailable" : probe ? "ok" : "no reply", ping_target());
    } else {
        printf("tunnel ping    off\n");
    }
    return 0;
}

static int cmd_keygen(void)
{
    char priv_b64[64], pub_b64[64];
    device_key(priv_b64, pub_b64);
    printf("%s\n%s\n", priv_b64, pub_b64);
    return 0;
}

/*
 * Убирает агента с камеры целиком: интерфейс, автозапуск, состояние и сам бинарь.
 *
 * Это не «сбросить подключение» — для этого есть store_forget_enrollment, оставляющий ключ и
 * claim-код на месте, — а «камеры в облаке больше нет». Порядок обратный установке: сначала уходит
 * то, что держит туннель, затем то, что его помнит, и последним файл, который всё это делал.
 *
 * Узел остаётся в реестре облака, и это не недоделка: у агента нет прав отзывать самого себя, и
 * появиться им незачем. Иначе камера, до которой кто-то дотянулся по SSH, выписывала бы себя из
 * чужого аккаунта сама. Запись убирает владелец, в панели.
 */
static int cmd_clean(void)
{
    /* Both spellings of each: whatever an older install left under streq_* goes too. */
    static const char *suffixes[] = {
        "node_id", "overlay_ip", "hub_endpoint", "hub_pubkey", "allowed_ips", "network_subnet",
        "hub_ip", "claim_code", "device_key", "api", "enrollment_token", "hostname", "kind", "iface",
        "subnet", "ca", "heartbeat", "ping_target", "ping_interval", "ping_timeout", "ping_fails",
        "events", "snapshot_url",
        NULL
    };
    char self[256];
    char cmd[320];
    char *sh[] = { "sh", "-c", NULL, NULL };
    ssize_t n;
    int i;

    /* Где мы лежим на самом деле: на камере это /usr/sbin/cerberus, на стенде — что угодно. */
    n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0)
        snprintf(self, sizeof self, "%s", DEFAULT_SELF_PATH);
    else
        self[n] = 0;

    runf("ip link del dev %s", g_cfg.iface);
    motion_hook_remove();
    if (unlink(INIT_SCRIPT) != 0 && errno != ENOENT)
        warn_("cannot remove %s: %s", INIT_SCRIPT, strerror(errno));

    for (i = 0; suffixes[i]; i++)
        store_unset_both(suffixes[i]);
    if (!store_is_uboot())
        unlink(store_file());
    unlink(g_cfg.seed);

    note("cleaned: %s and %s are gone, the stored material with them", INIT_SCRIPT, self);

    /*
     * Себя бинарь удаляет чужими руками и не сразу: работающий процесс переживёт unlink на любой
     * нормальной ФС, но на камере ФС бывает какая угодно, а половина удалённого агента хуже целого.
     * Три секунды — чтобы этот процесс успел закончиться и отдать код возврата тому, кто его звал.
     */
    snprintf(cmd, sizeof cmd, "(sleep 3; rm -f %s) >/dev/null 2>&1 &", self);
    sh[2] = cmd;
    run(sh, NULL, 0);
    return 0;
}

static void usage(void)
{
    fputs(
        "cerberus " CERBERUS_VERSION " — on-camera cloud agent for OpenIPC\n"
        "\n"
        "usage: cerberus [enroll|up|run|status|keygen|clean|motion|snapshot]\n"
        "  enroll   enroll into the coordinator and persist the join material (idempotent)\n"
        "  up       program the WireGuard interface from the stored material\n"
        "  run      enroll + up + watch the tunnel (the default; what the init script calls)\n"
        "  status   print what is stored, how old the handshake is, whether the tunnel answers\n"
        "  keygen   print the device keypair (private, public), creating it if needed\n"
        "  clean    remove the agent: interface, init script, stored material, the binary\n"
        "  motion   tell the running agent there is motion, as majestic's hook does\n"
        "  snapshot fetch one frame from CERBERUS_SNAPSHOT_URL into /tmp/cerberus-snapshot.jpg\n"
        "\n"
        "config (env; the old STREQ_* names are accepted too):\n"
        "  CERBERUS_API                coordinator base URL incl. /v1  [https://api.localhost/v1]\n"
        "  CERBERUS_ENROLLMENT_TOKEN   one-shot token from the owner\n"
        "  CERBERUS_KIND               node kind  [Camera]\n"
        "  CERBERUS_IFACE              WireGuard interface  [wg0]\n"
        "  CERBERUS_SUBNET             overlay CIDR routed to the hub  [10.80.0.0/16]\n"
        "  CERBERUS_CA                 CA file or dir for TLS  [/etc/ssl/certs/ca-certificates.crt]\n"
        "  CERBERUS_INSECURE=1         skip TLS verification (dev only)\n"
        "  CERBERUS_STORE              auto | uboot | file:/path  [auto] — holds the device key too\n"
        "  CERBERUS_KEYFILE            legacy key file, migrated into the store  [/etc/streq/wg.key]\n"
        "  CERBERUS_HEARTBEAT          seconds between heartbeats; 0 disables  [20]\n"
        "  CERBERUS_PING_TARGET        in-tunnel probe target  [the hub]\n"
        "  CERBERUS_PING_INTERVAL      seconds between probes; 0 disables  [60]\n"
        "  CERBERUS_PING_TIMEOUT       seconds one probe waits  [3]\n"
        "  CERBERUS_PING_FAILS         failed probes before the interface is rebuilt  [3]\n"
        "  CERBERUS_EVENTS=0           no motion hook, no events  [1]\n"
        "  CERBERUS_SNAPSHOT_URL       majestic's JPEG, user:pass@ allowed\n"
        "                              [http://127.0.0.1/image.jpg?width=640&height=360]\n"
        "  CERBERUS_MOTION_HOOK        the script majestic runs on motion  [/usr/sbin/motion.sh]\n"
        "  CERBERUS_MOTION_QUIET       seconds without motion that end an episode  [10]\n"
        "  CERBERUS_PIDFILE            where the hook finds the agent  [/var/run/cerberus.pid]\n"
        "  CERBERUS_SEED               randomness kept between starts  [/etc/cerberus/seed]\n"
        "  CERBERUS_RANDOM_DEV         the kernel's random device  [/dev/urandom]\n"
        "  CERBERUS_GETRANDOM=0        do not ask getrandom(2) first  [1]\n"
        "  CERBERUS_DRY_RUN=1          print ip/wg commands instead of running them\n",
        stderr);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "run";

    /* Kept for self_restart(): re-exec has to reproduce the argv we were started with. */
    g_argv = argv;

    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) { usage(); return 0; }

    cfg_load();

    /* Before anything reads the store: `clean` removes both spellings itself. */
    if (strcmp(cmd, "clean") != 0)
        store_migrate_legacy();

    if (!strcmp(cmd, "status")) return cmd_status();
    if (!strcmp(cmd, "keygen")) return cmd_keygen();
    if (!strcmp(cmd, "clean")) return cmd_clean();
    if (!strcmp(cmd, "enroll")) return enroll();
    if (!strcmp(cmd, "up")) return link_up();
    if (!strcmp(cmd, "motion")) return cmd_motion();
    if (!strcmp(cmd, "snapshot")) return cmd_snapshot();
    if (!strcmp(cmd, "run")) {
        catch_signals();
        /* Ненулевой ответ здесь бывает ровно один: попросили остановиться. Это не отказ. */
        if (enroll_with_retry() != 0) {
            note("stopping");
            return 0;
        }
        /* A failed bring-up is not fatal here, and this is the difference between a camera that can
         * be fixed remotely and one that cannot: the stored hub endpoint may no longer resolve (the
         * hub moved, its DNS name is gone), and the answer to that is a directive — which only
         * arrives if we stay alive to heartbeat for it. The overlay is not the path to the
         * coordinator; the internet is. */
        if (link_up() != 0)
            warn_("staying up without the overlay — a directive may still fix the material");
        return watch();
    }

    usage();
    return 2;
}
