/* warpforge_tunnel.c -- the mod's encrypted match tunnel, done natively.
 *
 * The mod (MatchTunnel.cs) listens on 127.0.0.1 and carries each connection the
 * game's match client makes to the revival server over TLS, because the game's
 * own Photon client speaks plain TCP. On Android that is a TcpListener, an
 * SslStream and the .NET thread pool. Here none of those are safe to lean on:
 * Mono NX has no SslStream backend, libnx's accept() can fail with errno left at
 * 0 (the runtime then took -1 for a socket and spun), and every spin cost thread
 * pool threads until thread creation failed and Mono gave up ("Press A to exit").
 *
 * So the loader replaces MatchTunnel.Start(host, port) with a call to
 * wf_tunnel_start below: one listener on 127.0.0.1, one small libnx thread per
 * connection that shuttles bytes between the local socket and a wf_tls
 * connection (warpforge_tls.c, mbedTLS) from a single poll loop. No Mono
 * sockets, no thread pool, nothing to exhaust.
 */
#include "config.h"
#if WF_MONO
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <switch.h>
#include "util.h"

extern void *wf_tls_connect(const char *host, int port, int timeout_ms, char *err, int errlen);
extern int   wf_tls_read(void *h, unsigned char *buf, int len);
extern int   wf_tls_write(void *h, const unsigned char *buf, int len);
extern void  wf_tls_close(void *h);
extern int   wf_tls_fd(void *h);
extern int   wf_tls_pending(void *h);

static Mutex  g_lock;
static int    g_listen_fd = -1;
static int    g_local_port;
static char   g_host[256];
static int    g_port;
static Thread g_accept_thread;
static int    g_live;             /* connections being carried */

#define MAX_CONNS 8
typedef struct { Thread thread; int used; volatile int done; int local; } conn_t;
static conn_t g_conns[MAX_CONNS];

/* Two threads per connection, one per direction. The single poll loop before
 * this only woke on the server side (libnx's poll did not report the loopback
 * socket readable), so the game's sends piled up until Photon's ping blocked
 * the main thread for 20 s at a time in battle. The TLS context is not
 * thread-safe, so the downstream thread waits for data with poll (outside the
 * lock) and both take the lock only around the actual TLS read/write. */
typedef struct {
  conn_t *c; void *tls; int local;
  Mutex lock;
  volatile int closing;
  long up, down;
  Thread down_thread;
} link_t;
extern void wf_tls_abort(void *h);

static void carry_down(void *arg) {           /* server -> game */
  link_t *l = arg;
  unsigned char buf[16384];
  unsigned n_logged = 0;
  const char *why = NULL;
  while (!l->closing) {
    if (!wf_tls_pending(l->tls)) {
      struct pollfd p = { .fd = wf_tls_fd(l->tls), .events = POLLIN };
      int r = poll(&p, 1, 1000);
      if (r < 0 && errno != EINTR) { why = "poll failed"; break; }
      if (r <= 0) continue;
    }
    mutexLock(&l->lock);
    int n = l->closing ? -1 : wf_tls_read(l->tls, buf, (int)sizeof buf);
    mutexUnlock(&l->lock);
    if (n == 0) { why = "the server closed it"; break; }
    if (n < 0) { if (!l->closing) why = "TLS read failed"; break; }
    l->down += n; if (++n_logged <= 3) debugPrintf("[tunnel] %d: server -> game %d bytes\n", l->local, n);
    int done = 0;
    while (done < n) {
      ssize_t w = send(l->local, buf + done, (size_t)(n - done), 0);
      if (w <= 0) { why = "send to the game failed"; break; }
      done += (int)w;
    }
    if (done < n) break;
  }
  if (why && !l->closing) debugPrintf("[tunnel] %d: %s\n", l->local, why);
  l->closing = 1;
  shutdown(l->local, SHUT_RDWR);            /* wakes the upstream thread's recv */
}

static void carry(void *arg) {                /* game -> server, and the connection's owner */
  conn_t *c = arg;
  int local = c->local;
  char host[256]; int port;
  mutexLock(&g_lock); strncpy(host, g_host, sizeof host - 1); host[sizeof host - 1] = 0; port = g_port; mutexUnlock(&g_lock);

  char err[256];
  void *tls = wf_tls_connect(host, port, 24 * 3600 * 1000, err, sizeof err);   /* reads block as long as a match lasts */
  if (!tls) {
    debugPrintf("[tunnel] match connection to %s:%d failed: %s\n", host, port, err);
    close(local);
    __atomic_fetch_sub(&g_live, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c->done, 1, __ATOMIC_SEQ_CST);
    return;
  }
  debugPrintf("[tunnel] match connection %d open to %s:%d\n", local, host, port);
  int one = 1; setsockopt(local, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  link_t *l = calloc(1, sizeof *l);
  l->c = c; l->tls = tls; l->local = local; mutexInit(&l->lock);
  int have_down = 0;
  Result rc = threadCreate(&l->down_thread, carry_down, l, NULL, 128 * 1024, 0x3B, -2);
  if (R_SUCCEEDED(rc)) rc = threadStart(&l->down_thread);
  if (R_SUCCEEDED(rc)) have_down = 1;
  else debugPrintf("[tunnel] %d: could not start the downstream thread (0x%x)\n", local, rc);

  unsigned char buf[16384];
  unsigned n_logged = 0;
  const char *why = "closed";
  while (have_down && !l->closing) {
    ssize_t n = recv(local, buf, sizeof buf, 0);
    if (n <= 0) { why = n == 0 ? "the game closed it" : "recv from the game failed"; break; }
    mutexLock(&l->lock);
    int w = l->closing ? -1 : wf_tls_write(tls, buf, (int)n);
    mutexUnlock(&l->lock);
    if (w < 0) { why = l->closing ? "the server side ended" : "TLS write failed"; break; }
    l->up += n; if (++n_logged <= 3) debugPrintf("[tunnel] %d: game -> server %d bytes\n", local, (int)n);
  }
  l->closing = 1;
  wf_tls_abort(tls);                          /* wakes the downstream thread's poll/read */
  if (have_down) { threadWaitForExit(&l->down_thread); threadClose(&l->down_thread); }
  debugPrintf("[tunnel] match connection %d ended: %s (%ld bytes up, %ld down)\n", local, why, l->up, l->down);
  wf_tls_close(tls);
  shutdown(local, SHUT_RDWR);
  close(local);
  free(l);
  __atomic_fetch_sub(&g_live, 1, __ATOMIC_SEQ_CST);
  __atomic_store_n(&c->done, 1, __ATOMIC_SEQ_CST);
}

/* Slots of connections that have ended are reclaimed (thread joined and closed)
 * before a new one is started; the accept thread is the only user of the table. */
static conn_t *conn_slot(void) {
  for (int i = 0; i < MAX_CONNS; i++)
    if (g_conns[i].used && __atomic_load_n(&g_conns[i].done, __ATOMIC_SEQ_CST)) {
      threadWaitForExit(&g_conns[i].thread);
      threadClose(&g_conns[i].thread);
      memset(&g_conns[i], 0, sizeof g_conns[i]);
    }
  for (int i = 0; i < MAX_CONNS; i++)
    if (!g_conns[i].used) { g_conns[i].used = 1; return &g_conns[i]; }
  return NULL;
}

static void accept_loop(void *arg) {
  (void)arg;
  for (;;) {
    struct sockaddr_in sa; socklen_t sl = sizeof sa;
    int fd = accept(g_listen_fd, (struct sockaddr *)&sa, &sl);
    if (fd < 0) {
      /* libnx leaves errno at 0 on some failures; none of them are fatal here */
      if (errno == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { svcSleepThread(20 * 1000000ULL); continue; }
      debugPrintf("[tunnel] accept failed (errno %d) -- the match tunnel is closed\n", errno);
      break;
    }
    if (sa.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) { close(fd); continue; }
    conn_t *c = conn_slot();
    if (!c) { debugPrintf("[tunnel] %d match connections open already; refusing another\n", MAX_CONNS); close(fd); continue; }
    c->local = fd;
    __atomic_fetch_add(&g_live, 1, __ATOMIC_SEQ_CST);
    Result rc = threadCreate(&c->thread, carry, c, NULL, 128 * 1024, 0x3B, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&c->thread);
    if (R_FAILED(rc)) {
      debugPrintf("[tunnel] could not start a connection thread (0x%x)\n", rc);
      close(fd); memset(c, 0, sizeof *c); __atomic_fetch_sub(&g_live, 1, __ATOMIC_SEQ_CST);
      continue;
    }
  }
  mutexLock(&g_lock);
  close(g_listen_fd); g_listen_fd = -1; g_local_port = 0;
  mutexUnlock(&g_lock);
}

/* Called from the loader in place of MatchTunnel.Start: returns the local port
 * the game's match client should connect to, or 0 if the listener could not be
 * made (the caller then connects to the server directly, unencrypted). */
int wf_tunnel_start(const char *host, int port) {
  if (!host || !*host || port <= 0) return 0;
  mutexLock(&g_lock);
  strncpy(g_host, host, sizeof g_host - 1); g_host[sizeof g_host - 1] = 0;
  g_port = port;
  if (g_listen_fd >= 0) { int p = g_local_port; mutexUnlock(&g_lock); return p; }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { mutexUnlock(&g_lock); debugPrintf("[tunnel] socket() failed (errno %d)\n", errno); return 0; }
  struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = 0, .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) } };
  if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 4) != 0) {
    debugPrintf("[tunnel] bind/listen failed (errno %d)\n", errno);
    close(fd); mutexUnlock(&g_lock); return 0;
  }
  socklen_t sl = sizeof sa;
  if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) { close(fd); mutexUnlock(&g_lock); return 0; }
  g_listen_fd = fd;
  g_local_port = ntohs(sa.sin_port);
  Result rc = threadCreate(&g_accept_thread, accept_loop, NULL, NULL, 64 * 1024, 0x3B, -2);
  if (R_SUCCEEDED(rc)) rc = threadStart(&g_accept_thread);
  if (R_FAILED(rc)) {
    debugPrintf("[tunnel] could not start the accept thread (0x%x)\n", rc);
    close(fd); g_listen_fd = -1; g_local_port = 0; mutexUnlock(&g_lock); return 0;
  }
  int p = g_local_port;
  mutexUnlock(&g_lock);
  debugPrintf("[tunnel] match tunnel listening on 127.0.0.1:%d -> %s:%d over TLS\n", p, host, port);
  return p;
}

#endif /* WF_MONO */
