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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
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
#define streq_sha256(in, len, out) mbedtls_sha256_ret((in), (len), (out), 0)
#else
#define streq_sha256(in, len, out) mbedtls_sha256((in), (len), (out), 0)
#endif

/* Release builds pass the tag in (`make VERSION=...`); a plain `make` reports the source version. */
#ifndef CERBERUS_VERSION
#define CERBERUS_VERSION "0.1.0"
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

/* Bound from STREQ_* env, same convention as the .NET agent (AgentOptions). */
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
};

static struct cfg g_cfg;

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

static void cfg_load(void)
{
    g_cfg.api = env_or("STREQ_API", "https://api.localhost/v1");
    g_cfg.token = env_or("STREQ_ENROLLMENT_TOKEN", "");
    g_cfg.kind = env_or("STREQ_KIND", "Camera");
    g_cfg.hostname = env_or("STREQ_HOSTNAME", NULL);
    g_cfg.iface = env_or("STREQ_IFACE", "wg0");
    g_cfg.subnet = env_or("STREQ_SUBNET", "10.80.0.0/16");
    g_cfg.ca = env_or("STREQ_CA", "/etc/ssl/certs/ca-certificates.crt");
    g_cfg.store = env_or("STREQ_STORE", "auto");
    g_cfg.keyfile = env_or("STREQ_KEYFILE", "/etc/streq/wg.key");
    g_cfg.insecure = atoi(env_or("STREQ_INSECURE", "0"));
    g_quiet = atoi(env_or("STREQ_QUIET", "0"));
    g_cfg.dry_run = atoi(env_or("STREQ_DRY_RUN", "0"));
    g_cfg.watch_interval = atoi(env_or("STREQ_WATCH_INTERVAL", "30"));
    g_cfg.stale_after = atoi(env_or("STREQ_STALE_AFTER", "180"));

    /* The coordinator's contract: 20s by default, never faster than 5. */
    g_cfg.heartbeat = atoi(env_or("STREQ_HEARTBEAT", "20"));
    if (g_cfg.heartbeat > 0 && g_cfg.heartbeat < 5) g_cfg.heartbeat = 5;
    if (g_cfg.watch_interval < 1) g_cfg.watch_interval = 1;

    /* The in-tunnel probe. Slower than the handshake check on purpose: it costs a packet round trip
     * over the overlay, and what it is looking for (a path that is up on paper and dead in fact) does
     * not appear between two ticks. */
    g_cfg.ping_target = env_or("STREQ_PING_TARGET", "");
    g_cfg.ping_interval = atoi(env_or("STREQ_PING_INTERVAL", "60"));
    g_cfg.ping_timeout = atoi(env_or("STREQ_PING_TIMEOUT", "3"));
    g_cfg.ping_fails = atoi(env_or("STREQ_PING_FAILS", "3"));
    if (g_cfg.ping_interval > 0 && g_cfg.ping_interval < 10) g_cfg.ping_interval = 10;
    if (g_cfg.ping_timeout < 1) g_cfg.ping_timeout = 1;
    if (g_cfg.ping_fails < 1) g_cfg.ping_fails = 1;

    /* Ждать координатор, а не умирать. См. enroll_with_retry. */
    g_cfg.retry_min = atoi(env_or("STREQ_RETRY_MIN", "5"));
    g_cfg.retry_max = atoi(env_or("STREQ_RETRY_MAX", "300"));
    if (g_cfg.retry_min < 1) g_cfg.retry_min = 1;
    if (g_cfg.retry_max < g_cfg.retry_min) g_cfg.retry_max = g_cfg.retry_min;
}

/* ------------------------------------------------------------------ crypto */

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;

static void crypto_init(void)
{
    const char *pers = "cerberus";
#if defined(MBEDTLS_PSA_CRYPTO_C)
    psa_crypto_init();
#endif
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        die("no entropy source");
}

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
    if (streq_sha256((const unsigned char *)in, strlen(in), d) != 0)
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
    if (rc == 0) rc = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, mbedtls_ctr_drbg_random, &g_drbg);
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
    if (mbedtls_ctr_drbg_random(&g_drbg, r, sizeof r) != 0)
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

static const char *store_file(void)
{
    if (!strncmp(g_cfg.store, "file:", 5)) return g_cfg.store + 5;
    return "/etc/streq/agent.env";
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

static void store_set(const char *key, const char *value)
{
    const char *path;
    char tmp[512], line[1024];
    size_t klen = strlen(key);
    FILE *in, *out;

    if (store_is_uboot()) {
        char *argv[] = { "fw_setenv", (char *)key, (char *)value, NULL };
        if (run(argv, NULL, 0) != 0)
            die("fw_setenv %s failed — is /etc/fw_env.config right?", key);
        return;
    }

    /* Rewrite the file with this key replaced. Small and rare enough that a full rewrite beats
     * carrying an in-memory model of it. */
    path = store_file();
    mkparent(path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    out = fopen(tmp, "w");
    if (!out) die("cannot write %s: %s", tmp, strerror(errno));
    in = fopen(path, "r");
    if (in) {
        while (fgets(line, sizeof line, in)) {
            if (!strncmp(line, key, klen) && line[klen] == '=') continue;
            fputs(line, out);
        }
        fclose(in);
    }
    fprintf(out, "%s=%s\n", key, value);
    fclose(out);
    if (chmod(tmp, 0600) != 0) warn_("chmod %s: %s", tmp, strerror(errno));
    if (rename(tmp, path) != 0) die("cannot replace %s: %s", path, strerror(errno));
}

/*
 * Forgets the enrolment, keeping the device's own key and claim code. Called when the coordinator
 * says it has never heard of this node: it was revoked, or rebuilt from under us. Enrolment is
 * skipped whenever a node id is stored, so leaving it in place would turn every later boot into the
 * same dead end — the same reasoning as AgentHost.TryDeleteCache in the .NET agent.
 */
static void store_forget_enrollment(void)
{
    static const char *keys[] = {
        "streq_node_id", "streq_overlay_ip", "streq_hub_endpoint",
        "streq_hub_pubkey", "streq_allowed_ips", "streq_network_subnet",
        "streq_hub_ip", NULL
    };
    int i;

    for (i = 0; keys[i]; i++) {
        if (store_is_uboot()) {
            char *argv[] = { "fw_setenv", (char *)keys[i], NULL };   /* no value = unset */
            run(argv, NULL, 0);
        } else {
            store_set(keys[i], "");
        }
    }
    warn_("enrolment discarded; restart with a fresh STREQ_ENROLLMENT_TOKEN to join again");
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

/* https://host[:port]/path — the only URL shape the coordinator is ever configured with. */
static int url_split(const char *url, int *tls, char *host, size_t host_sz,
                     char *port, size_t port_sz, char *path, size_t path_sz)
{
    const char *p, *slash;
    char *colon;

    if (!strncmp(url, "https://", 8)) { *tls = 1; p = url + 8; snprintf(port, port_sz, "443"); }
    else if (!strncmp(url, "http://", 7)) { *tls = 0; p = url + 7; snprintf(port, port_sz, "80"); }
    else return -1;

    slash = strchr(p, '/');
    snprintf(path, path_sz, "%s", slash ? slash : "/");
    snprintf(host, host_sz, "%.*s", slash ? (int)(slash - p) : (int)strlen(p), p);

    colon = strrchr(host, ':');
    if (colon && !strchr(host, ']')) {   /* not a bracketed v6 literal */
        snprintf(port, port_sz, "%s", colon + 1);
        *colon = 0;
    }
    return 0;
}

/*
 * One request, one connection, `Connection: close` — so the body is simply "everything until EOF"
 * and neither Content-Length nor chunked framing has to be parsed. The agent makes at most one HTTP
 * call per boot; keep-alive would buy nothing and cost a de-chunker.
 */
static int http_post_json(const char *url, const char *body, char *resp, size_t resp_sz, int *status)
{
    struct conn c;
    char host[256], port[16], path[512], head[1024];
    unsigned char buf[4096];
    char *hdr_end;
    size_t total = 0;
    int rc, n;

    memset(&c, 0, sizeof c);
    if (url_split(url, &c.tls, host, sizeof host, port, sizeof port, path, sizeof path) != 0)
        die("unsupported URL: %s", url);

    mbedtls_net_init(&c.net);
    if ((rc = mbedtls_net_connect(&c.net, host, port, MBEDTLS_NET_PROTO_TCP)) != 0) {
        warn_("connect %s:%s failed (-0x%04x)", host, port, (unsigned)-rc);
        return -1;
    }

    if (c.tls) {
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
            warn_("TLS verification disabled (STREQ_INSECURE=1)");
            mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_NONE);
        } else {
            struct stat st;
            int loaded = -1;
            if (stat(g_cfg.ca, &st) == 0)
                loaded = S_ISDIR(st.st_mode)
                    ? mbedtls_x509_crt_parse_path(&c.ca, g_cfg.ca)
                    : mbedtls_x509_crt_parse_file(&c.ca, g_cfg.ca);
            if (loaded < 0)
                die("no usable CA trust store at %s (set STREQ_CA)", g_cfg.ca);
            mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&c.conf, &c.ca, NULL);
        }

        mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, &g_drbg);
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
        note("TLS ok: %s, %s", mbedtls_ssl_get_version(&c.ssl), mbedtls_ssl_get_ciphersuite(&c.ssl));
    }

    snprintf(head, sizeof head,
             "POST %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: cerberus/" CERBERUS_VERSION "\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n\r\n",
             path, host, (unsigned)strlen(body));

    if (conn_send(&c, head, strlen(head)) != 0 || conn_send(&c, body, strlen(body)) != 0) {
        warn_("sending the request failed");
        conn_close(&c);
        return -1;
    }

    while (total + 1 < resp_sz) {
        n = conn_recv(&c, buf, sizeof buf);
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) break;
        if (n < 0) break;
        if (total + (size_t)n + 1 > resp_sz) n = (int)(resp_sz - total - 1);
        memcpy(resp + total, buf, (size_t)n);
        total += (size_t)n;
    }
    resp[total] = 0;
    conn_close(&c);

    if (total == 0 || strncmp(resp, "HTTP/1.", 7) != 0) {
        warn_("no HTTP response");
        return -1;
    }
    *status = atoi(resp + 9);

    hdr_end = strstr(resp, "\r\n\r\n");
    if (hdr_end) memmove(resp, hdr_end + 4, strlen(hdr_end + 4) + 1);
    return 0;
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
 * — while `streq_node_id` next to it in the U-Boot environment survives. The node would come back
 * with an identity the hub has a peer for and no key to prove it, and enrolment is skipped whenever a
 * node id is stored: not "re-enrollable", *stuck*. Key and node id have to share a lifetime, so they
 * share a store.
 *
 * An older key sitting in STREQ_KEYFILE is migrated in on sight, once. It is left on disk rather than
 * deleted: the reflash that motivates all of this will take it anyway, and a delete racing a failed
 * fw_setenv would be the one unrecoverable outcome here.
 */
static void device_key(char priv_b64[64], char pub_b64[64])
{
    unsigned char priv[32], pub[32];
    char stored[128], line[128];
    size_t olen = 0;
    FILE *f;

    if (store_get("streq_device_key", stored, sizeof stored) && stored[0]) {
        if (b64_decode(stored, priv, sizeof priv, &olen) != 0 || olen != 32)
            die("streq_device_key is not a base64 WireGuard key");
        b64(priv, sizeof priv, priv_b64, 64);   /* re-encode: normalises whatever was stored */
    } else if ((f = fopen(g_cfg.keyfile, "r")) != NULL) {
        int ok = fgets(line, sizeof line, f) != NULL;
        fclose(f);
        line[strcspn(line, "\r\n")] = 0;
        if (!ok || b64_decode(line, priv, sizeof priv, &olen) != 0 || olen != 32)
            die("%s is not a base64 WireGuard key", g_cfg.keyfile);
        b64(priv, sizeof priv, priv_b64, 64);
        store_set("streq_device_key", priv_b64);
        note("migrated the device key from %s into the store — it now survives a firmware update",
             g_cfg.keyfile);
    } else {
        if (mbedtls_ctr_drbg_random(&g_drbg, priv, sizeof priv) != 0) die("rng failed");
        wg_clamp(priv);
        b64(priv, sizeof priv, priv_b64, 64);
        store_set("streq_device_key", priv_b64);
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
    snprintf(path, path_sz, "%s", env_or("STREQ_KEY_TMP", "/tmp/.streq-wg.key"));

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
    if (store_get("streq_claim_code", code, code_sz) && code[0]) return;
    claim_code_new(fresh);
    store_set("streq_claim_code", fresh);
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

    if (store_get("streq_node_id", node, sizeof node)) {
        store_get("streq_overlay_ip", ip, sizeof ip);
        note("already enrolled as %s (overlay %s)", node, ip[0] ? ip : "?");
        return 0;
    }

    if (!*g_cfg.token)
        die("not enrolled and no STREQ_ENROLLMENT_TOKEN set");

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
     * работает STREQ_SUBNET, как и раньше. */
    if (!json_str(resp, "networkSubnet", subnet, sizeof subnet)) subnet[0] = 0;
    if (!json_str(resp, "hubOverlayIp", hubip, sizeof hubip)) hubip[0] = 0;

    /* Persist before touching the interface: a camera that got a peer on the hub and then lost the
     * material would re-enroll on the next boot with a token already spent. */
    store_set("streq_node_id", node);
    store_set("streq_overlay_ip", ip);
    store_set("streq_hub_endpoint", ep);
    store_set("streq_hub_pubkey", hubkey);
    store_set("streq_allowed_ips", allowed);
    if (subnet[0]) store_set("streq_network_subnet", subnet);
    if (hubip[0]) store_set("streq_hub_ip", hubip);

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
 * Подсеть, в которой стоит этот узел: из стора, если координатор её назвал, иначе STREQ_SUBNET.
 *
 * With one network per instance these are the same string. With tenants they are not: the node's
 * own /24 is what it may claim on the interface, and STREQ_SUBNET is a guess made by whoever wrote
 * the u-boot environment — usually the instance-wide pool, which belongs to everybody.
 */
static const char *node_subnet(void)
{
    static char subnet[64];
    if (store_get("streq_network_subnet", subnet, sizeof subnet) && subnet[0])
        return subnet;
    return g_cfg.subnet;
}

/* Overlay-адрес хаба, если координатор его назвал. Пусто — хаб внутри нашей же подсети. */
static const char *hub_overlay_ip(void)
{
    static char hubip[64];
    if (store_get("streq_hub_ip", hubip, sizeof hubip) && hubip[0])
        return hubip;
    return "";
}

static int link_apply(int flush)
{
    char ip[64], ep[256], hubkey[64], keypath[256];
    const char *subnet, *hubip;
    int rc;

    if (!store_get("streq_overlay_ip", ip, sizeof ip) ||
        !store_get("streq_hub_endpoint", ep, sizeof ep) ||
        !store_get("streq_hub_pubkey", hubkey, sizeof hubkey)) {
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
 * узлу он больше ни для чего не нужен — поэтому адрес выводится из подсети, а STREQ_PING_TARGET
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
        (!store_get("streq_hub_endpoint", cur, sizeof cur) || strcmp(cur, ep) != 0)) {
        note("hub endpoint rotated: %s -> %s", cur[0] ? cur : "(none)", ep);
        store_set("streq_hub_endpoint", ep);
        rotated = 1;
    }
    if (json_str(json, "hubPublicKey", hubkey, sizeof hubkey) &&
        (!store_get("streq_hub_pubkey", cur, sizeof cur) || strcmp(cur, hubkey) != 0)) {
        note("hub public key rotated");
        store_set("streq_hub_pubkey", hubkey);
        rotated = 1;
    }
    if (json_str(json, "overlayIp", ip, sizeof ip) &&
        (!store_get("streq_overlay_ip", cur, sizeof cur) || strcmp(cur, ip) != 0)) {
        note("overlay address rotated: %s -> %s", cur[0] ? cur : "(none)", ip);
        store_set("streq_overlay_ip", ip);
        rotated = 1;
    }
    if (json_str(json, "allowedIps", allowed, sizeof allowed))
        store_set("streq_allowed_ips", allowed);
    if (json_str(json, "networkSubnet", fresh, sizeof fresh) && fresh[0] &&
        (!store_get("streq_network_subnet", cur, sizeof cur) || strcmp(cur, fresh) != 0)) {
        note("network subnet rotated: %s -> %s", cur[0] ? cur : "(none)", fresh);
        store_set("streq_network_subnet", fresh);
        rotated = 1;
    }
    if (json_str(json, "hubOverlayIp", fresh, sizeof fresh) && fresh[0] &&
        (!store_get("streq_hub_ip", cur, sizeof cur) || strcmp(cur, fresh) != 0)) {
        note("hub overlay address rotated: %s -> %s", cur[0] ? cur : "(none)", fresh);
        store_set("streq_hub_ip", fresh);
        rotated = 1;
    }

    if (rotated && link_apply(1) != 0)
        warn_("could not apply the rotated join material; the stale-handshake path will retry");

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

    if (!store_get("streq_hub_endpoint", ep, sizeof ep) ||
        !store_get("streq_hub_pubkey", hubkey, sizeof hubkey))
        return 1;
    store_get("streq_node_id", node, sizeof node);
    snprintf(beat_url, sizeof beat_url, "%s/nodes/%s/heartbeat", g_cfg.api, node);

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
                store_get("streq_hub_endpoint", ep, sizeof ep);
                store_get("streq_hub_pubkey", hubkey, sizeof hubkey);
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

        if (!g_stop) sleep((unsigned)tick);
    }
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
    printf("node           %s\n", store_get("streq_node_id", v, sizeof v) ? v : "(not enrolled)");
    printf("device key     %s\n", store_get("streq_device_key", v, sizeof v) ? "in the store" : "-");
    printf("overlay ip     %s\n", store_get("streq_overlay_ip", v, sizeof v) ? v : "-");
    printf("hub endpoint   %s\n", store_get("streq_hub_endpoint", v, sizeof v) ? v : "-");
    printf("hub public key %s\n", store_get("streq_hub_pubkey", v, sizeof v) ? v : "-");
    printf("network subnet %s%s\n", node_subnet(),
           store_get("streq_network_subnet", v, sizeof v) ? "" : " (из STREQ_SUBNET)");
    printf("hub overlay ip %s\n", *hub_overlay_ip() ? hub_overlay_ip() : "- (внутри своей подсети)");
    printf("claim code     %s\n", store_get("streq_claim_code", v, sizeof v) ? v : "-");
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
    static const char *keys[] = {
        "streq_node_id", "streq_overlay_ip", "streq_hub_endpoint", "streq_hub_pubkey",
        "streq_allowed_ips", "streq_network_subnet", "streq_hub_ip", "streq_claim_code",
        "streq_device_key", "streq_api", "streq_enrollment_token", "streq_hostname",
        "streq_kind", "streq_iface", "streq_subnet", "streq_ca", "streq_heartbeat",
        "streq_ping_target", "streq_ping_interval", "streq_ping_timeout", "streq_ping_fails",
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
    if (unlink(INIT_SCRIPT) != 0 && errno != ENOENT)
        warn_("cannot remove %s: %s", INIT_SCRIPT, strerror(errno));

    for (i = 0; keys[i]; i++) {
        if (store_is_uboot()) {
            char *argv[] = { "fw_setenv", (char *)keys[i], NULL };   /* без значения — сброс */
            run(argv, NULL, 0);
        } else {
            store_set(keys[i], "");
        }
    }
    if (!store_is_uboot())
        unlink(store_file());

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
        "cerberus " CERBERUS_VERSION " — on-camera Streq agent\n"
        "\n"
        "usage: cerberus [enroll|up|run|status|keygen]\n"
        "  enroll   enroll into the coordinator and persist the join material (idempotent)\n"
        "  up       program the WireGuard interface from the stored material\n"
        "  run      enroll + up + watch the tunnel (the default; what the init script calls)\n"
        "  status   print what is stored, how old the handshake is, whether the tunnel answers\n"
        "  keygen   print the device keypair (private, public), creating it if needed\n"
        "\n"
        "config (env):\n"
        "  STREQ_API                coordinator base URL incl. /v1  [https://api.localhost/v1]\n"
        "  STREQ_ENROLLMENT_TOKEN   one-shot token from the owner\n"
        "  STREQ_KIND               node kind  [Camera]\n"
        "  STREQ_IFACE              WireGuard interface  [wg0]\n"
        "  STREQ_SUBNET             overlay CIDR routed to the hub  [10.80.0.0/16]\n"
        "  STREQ_CA                 CA file or dir for TLS  [/etc/ssl/certs/ca-certificates.crt]\n"
        "  STREQ_INSECURE=1         skip TLS verification (dev only)\n"
        "  STREQ_STORE              auto | uboot | file:/path  [auto] — holds the device key too\n"
        "  STREQ_KEYFILE            legacy key file, migrated into the store  [/etc/streq/wg.key]\n"
        "  STREQ_HEARTBEAT          seconds between heartbeats; 0 disables  [20]\n"
        "  STREQ_PING_TARGET        in-tunnel probe target  [first address of STREQ_SUBNET]\n"
        "  STREQ_PING_INTERVAL      seconds between probes; 0 disables  [60]\n"
        "  STREQ_PING_TIMEOUT       seconds one probe waits  [3]\n"
        "  STREQ_PING_FAILS         failed probes before the interface is rebuilt  [3]\n"
        "  STREQ_DRY_RUN=1          print ip/wg commands instead of running them\n",
        stderr);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "run";

    /* Kept for self_restart(): re-exec has to reproduce the argv we were started with. */
    g_argv = argv;

    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) { usage(); return 0; }

    cfg_load();
    crypto_init();

    if (!strcmp(cmd, "status")) return cmd_status();
    if (!strcmp(cmd, "keygen")) return cmd_keygen();
    if (!strcmp(cmd, "clean")) return cmd_clean();
    if (!strcmp(cmd, "enroll")) return enroll();
    if (!strcmp(cmd, "up")) return link_up();
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
