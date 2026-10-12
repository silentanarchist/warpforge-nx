/* warpforge_tls.c -- a TLS client for the mod's managed code (Mono NX has no TLS).
 *
 * Mono NX's class library has SslStream but no native crypto behind it
 * (DllNotFoundException: libSystem.Security.Cryptography.Native.OpenSsl), so the
 * mod fell back to plain http and the Revival server, which only accepts encrypted
 * connections, refused it (403 EncryptionRequired). This gives managed code one
 * blocking TLS connection per handle, built on mbedTLS from devkitPro's portlibs:
 *
 *   h = wf_tls_connect(host, port, timeout_ms, err, errlen)   DNS + TCP + handshake
 *   wf_tls_read / wf_tls_write                               blocking, like recv/send
 *   wf_tls_close(h)
 *
 * The server certificate must chain to the roots the wrapper already trusts for
 * the game (warpforge_cacerts.c: ISRG Root X1/X2) and match the host name.
 * Managed side: nxloader's NxTls (a Stream) and NxHttp (HttpClient for https).
 *
 * MIT.
 */
#include "config.h"
#if WF_MONO
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
/* the net_sockets module's error codes (its header is not always installed) */
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED  -0x004E
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED  -0x004C
#endif
#ifndef MBEDTLS_ERR_NET_CONN_RESET
#define MBEDTLS_ERR_NET_CONN_RESET   -0x0050
#endif
#include "util.h"
#include "warpforge_cacerts.h"

typedef struct {
  int fd;
  int timeout_ms;
  mbedtls_ssl_context ssl;
} wf_tls_t;

static mbedtls_ssl_config      g_conf;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt        g_roots;
static int g_ready;
static Mutex g_init_lock;

static int entropy(void *ctx, unsigned char *out, size_t len) {
  (void)ctx; randomGet(out, len); return 0;
}

static int init_once(char *err, int errlen) {
  mutexLock(&g_init_lock);
  if (g_ready) { mutexUnlock(&g_init_lock); return 0; }
  mbedtls_ctr_drbg_init(&g_drbg);
  mbedtls_x509_crt_init(&g_roots);
  mbedtls_ssl_config_init(&g_conf);
  int rc = mbedtls_ctr_drbg_seed(&g_drbg, entropy, NULL, (const unsigned char *)"warpforge_nx", 12);
  unsigned loaded = 0;
  for (unsigned i = 0; rc == 0 && i < wf_cacerts_n; i++)
    if (mbedtls_x509_crt_parse_der(&g_roots, wf_cacerts[i].der, wf_cacerts[i].len) == 0) loaded++;
  if (rc == 0) rc = mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc != 0) {
    snprintf(err, (size_t)errlen, "TLS setup failed (%d)", rc);
    mutexUnlock(&g_init_lock);
    return -1;
  }
  mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&g_conf, &g_roots, NULL);
  mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
  g_ready = 1;
  mutexUnlock(&g_init_lock);
  debugPrintf("[tls] mbedTLS ready, %u root certificate(s)\n", loaded);
  return 0;
}

/* bio callbacks: plain blocking socket with a poll timeout */
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
  wf_tls_t *t = ctx;
  ssize_t n = send(t->fd, buf, len, 0);
  if (n >= 0) return (int)n;
  return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}
static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
  wf_tls_t *t = ctx;
  if (t->timeout_ms > 0) {
    struct pollfd p = { .fd = t->fd, .events = POLLIN };
    int r = poll(&p, 1, t->timeout_ms);
    if (r == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (r < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
  }
  ssize_t n = recv(t->fd, buf, len, 0);
  if (n > 0) return (int)n;
  if (n == 0) return MBEDTLS_ERR_NET_CONN_RESET;
  return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

static void describe(int rc, const char *what, char *err, int errlen) {
  char m[160]; mbedtls_strerror(rc, m, sizeof m);
  snprintf(err, (size_t)errlen, "%s: %s (-0x%04x)", what, m, (unsigned)-rc);
}

void *wf_tls_connect(const char *host, int port, int timeout_ms, char *err, int errlen) {
  if (!err || errlen <= 0) { static char dummy[2]; err = dummy; errlen = 1; }
  err[0] = 0;
  if (init_once(err, errlen) < 0) return NULL;

  char portstr[8]; snprintf(portstr, sizeof portstr, "%d", port);
  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  int g = getaddrinfo(host, portstr, &hints, &res);
  if (g != 0 || !res) { snprintf(err, (size_t)errlen, "could not resolve %s (%d)", host, g); return NULL; }

  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) { freeaddrinfo(res); snprintf(err, (size_t)errlen, "socket() failed (%d)", errno); return NULL; }
  if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    int e = errno; freeaddrinfo(res); close(fd);
    snprintf(err, (size_t)errlen, "could not connect to %s:%d (errno %d)", host, port, e);
    return NULL;
  }
  freeaddrinfo(res);
  int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  wf_tls_t *t = calloc(1, sizeof *t);
  t->fd = fd; t->timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
  mbedtls_ssl_init(&t->ssl);
  int rc = mbedtls_ssl_setup(&t->ssl, &g_conf);
  if (rc == 0) rc = mbedtls_ssl_set_hostname(&t->ssl, host);
  if (rc != 0) { describe(rc, "TLS setup", err, errlen); goto fail; }
  mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
  while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0) {
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
    if (flags) {
      char why[256]; mbedtls_x509_crt_verify_info(why, sizeof why, "", flags);
      for (char *p = why; *p; p++) if (*p == '\n') *p = ' ';
      snprintf(err, (size_t)errlen, "certificate of %s refused: %s", host, why);
    } else describe(rc, "TLS handshake", err, errlen);
    goto fail;
  }
  {
    static int logged;
    if (logged < 3) { logged++; debugPrintf("[tls] connected to %s:%d (%s, %s)\n", host, port,
                       mbedtls_ssl_get_version(&t->ssl), mbedtls_ssl_get_ciphersuite(&t->ssl)); }
  }
  return t;
fail:
  debugPrintf("[tls] %s\n", err);
  mbedtls_ssl_free(&t->ssl);
  close(fd);
  free(t);
  return NULL;
}

/* >0 bytes, 0 = closed by the server, <0 = error */
int wf_tls_read(void *h, unsigned char *buf, int len) {
  wf_tls_t *t = h;
  if (!t || len <= 0) return -1;
  for (;;) {
    int r = mbedtls_ssl_read(&t->ssl, buf, (size_t)len);
    if (r > 0) return r;
    if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_NET_CONN_RESET) return 0;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
    if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
    return r;
  }
}

/* all `len` bytes written: returns len; <0 = error */
int wf_tls_write(void *h, const unsigned char *buf, int len) {
  wf_tls_t *t = h;
  if (!t) return -1;
  int done = 0;
  while (done < len) {
    int r = mbedtls_ssl_write(&t->ssl, buf + done, (size_t)(len - done));
    if (r > 0) { done += r; continue; }
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    return r;
  }
  return done;
}

void wf_tls_close(void *h) {
  wf_tls_t *t = h;
  if (!t) return;
  mbedtls_ssl_close_notify(&t->ssl);
  mbedtls_ssl_free(&t->ssl);
  close(t->fd);
  free(t);
}

/* For cancellation: makes a blocked read/write return at once (the managed
 * side calls it from another thread, then wf_tls_close). */
void wf_tls_abort(void *h) {
  wf_tls_t *t = h;
  if (t) shutdown(t->fd, SHUT_RDWR);
}

#endif /* WF_MONO */
