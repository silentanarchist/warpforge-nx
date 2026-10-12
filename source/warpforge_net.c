/* warpforge_net.c -- real BSD sockets for the game, over libnx's socket driver.
 *
 * WHY
 * Every port in this lineage was an offline game, so the substrate stubs the
 * socket layer: socket() fails with EAFNOSUPPORT and getaddrinfo() answers
 * "no such name". Warpforge is a client of the Revival server, and the first
 * hardware run stopped at its "Error connecting to server" dialog for exactly
 * that reason. This file replaces the stubs with the Switch's own network
 * stack (bsd:u, initialised by socketInitializeDefault() in main()).
 *
 * TWO ABIs
 * The game is a Linux/bionic binary; the Switch's C library is BSD-flavoured.
 * They disagree on:
 *   - struct sockaddr: Linux has a 16-bit family first; BSD has a length byte
 *     then an 8-bit family. Every address crossing the boundary is converted.
 *   - constants: SOL_SOCKET (1 vs 0xffff), the SO_* option numbers, AF_INET6
 *     (10 vs 28), MSG_* flags, TCP_KEEP*, IPV6_V6ONLY, O_NONBLOCK (0x800 vs
 *     0x4000), the AI_* and EAI_* families.
 *   - errno: the game reads errno through newlib's __errno (the substrate maps
 *     it straight through), so values are newlib's -- but the few it tests by
 *     number for socket state (EINPROGRESS, EWOULDBLOCK, EISCONN, ENOTCONN,
 *     EALREADY, EADDRINUSE, ENETUNREACH) differ, so those are translated to the
 *     Linux numbers after every call.
 *   - struct addrinfo has the same field order on both; only the sockaddr it
 *     points at differs. getaddrinfo() builds its own Linux-layout list and
 *     freeaddrinfo() frees that.
 *   - struct pollfd and the POLL* bits are identical.
 * IPv4 only: the Switch resolver answers A records and the Revival server is
 * reachable over IPv4; AF_INET6 requests are refused with EAFNOSUPPORT.
 *
 * Which descriptors are sockets is tracked in a bitmap so fcntl/ioctl/poll can
 * tell them from the file layer's fake descriptors.
 *
 * MIT.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <switch.h>
#include "util.h"
#include "so_util.h"

/* ------------------------------------------------------------ Linux ABI */
#define L_AF_INET      2
#define L_AF_INET6     10
#define L_SOL_SOCKET   1
#define L_SO_REUSEADDR 2
#define L_SO_TYPE      3
#define L_SO_ERROR     4
#define L_SO_BROADCAST 6
#define L_SO_SNDBUF    7
#define L_SO_RCVBUF    8
#define L_SO_KEEPALIVE 9
#define L_SO_LINGER    13
#define L_SO_REUSEPORT 15
#define L_SO_RCVTIMEO  20
#define L_SO_SNDTIMEO  21
#define L_IPPROTO_IPV6 41
#define L_IPV6_V6ONLY  26
#define L_TCP_KEEPIDLE 4
#define L_TCP_KEEPINTVL 5
#define L_TCP_KEEPCNT  6
#define L_MSG_OOB      1
#define L_MSG_PEEK     2
#define L_MSG_WAITALL  0x100
#define L_MSG_DONTWAIT 0x40
#define L_MSG_NOSIGNAL 0x4000
#define L_O_NONBLOCK   0x800
#define L_F_GETFL      3
#define L_F_SETFL      4
#define L_FIONBIO      0x5421
#define L_AI_PASSIVE     1
#define L_AI_CANONNAME   2
#define L_AI_NUMERICHOST 4
#define L_EAI_NONAME   (-2)
#define L_EAI_AGAIN    (-3)
#define L_EAI_FAIL     (-4)
#define L_EAI_FAMILY   (-6)
#define L_EAI_MEMORY   (-10)
#define L_EAI_SYSTEM   (-11)
/* Linux errno numbers the game compares against */
#define L_EAGAIN       11
#define L_EINPROGRESS  115
#define L_EALREADY     114
#define L_EISCONN      106
#define L_ENOTCONN     107
#define L_EADDRINUSE   98
#define L_ENETUNREACH  101
#define L_EAFNOSUPPORT 97
#define L_ENOTSOCK     88
#define L_EMSGSIZE     90
#define L_ETIMEDOUT    110
#define L_ECONNREFUSED 111
#define L_ECONNRESET   104
#define L_EHOSTUNREACH 113

struct l_sockaddr_in { uint16_t family; uint16_t port; uint32_t addr; uint8_t zero[8]; };
struct l_addrinfo {
  int ai_flags, ai_family, ai_socktype, ai_protocol;
  uint32_t ai_addrlen;
  char *ai_canonname;
  struct l_sockaddr_in *ai_addr;
  struct l_addrinfo *ai_next;
};
struct l_pollfd { int fd; short events, revents; };
struct l_linger { int l_onoff, l_linger; };

/* ------------------------------------------------------------- tracking */
static uint8_t g_is_sock[1024 / 8];
static int is_sock(int fd) { return fd >= 0 && fd < 1024 && (g_is_sock[fd >> 3] >> (fd & 7)) & 1; }
static void mark_sock(int fd, int on) {
  if (fd < 0 || fd >= 1024) return;
  if (on) g_is_sock[fd >> 3] |= (uint8_t)(1u << (fd & 7));
  else    g_is_sock[fd >> 3] &= (uint8_t)~(1u << (fd & 7));
}
int wf_net_is_socket(int fd) { return is_sock(fd); }
void wf_net_forget(int fd) { mark_sock(fd, 0); }

static int lerrno(int e) {
  switch (e) {
    case EINPROGRESS:  return L_EINPROGRESS;
    case EALREADY:     return L_EALREADY;
    case EISCONN:      return L_EISCONN;
    case ENOTCONN:     return L_ENOTCONN;
    case EADDRINUSE:   return L_EADDRINUSE;
    case ENETUNREACH:  return L_ENETUNREACH;
    case EAFNOSUPPORT: return L_EAFNOSUPPORT;
    case ENOTSOCK:     return L_ENOTSOCK;
    case EMSGSIZE:     return L_EMSGSIZE;
    case ETIMEDOUT:    return L_ETIMEDOUT;
    case ECONNREFUSED: return L_ECONNREFUSED;
    case ECONNRESET:   return L_ECONNRESET;
    case EHOSTUNREACH: return L_EHOSTUNREACH;
    case EWOULDBLOCK:  return L_EAGAIN;
    default:           return e;
  }
}
#define FAIL(e) do { errno = lerrno(e); return -1; } while (0)
static long ret_l(long r) { if (r < 0) errno = lerrno(errno); return r; }
static int  ret_i(int r)  { if (r < 0) errno = lerrno(errno); return r; }

/* ------------------------------------------------------------ addresses */
static int to_bsd(const void *la, unsigned llen, struct sockaddr_in *out) {
  if (!la || llen < sizeof(struct l_sockaddr_in)) return -1;
  const struct l_sockaddr_in *l = la;
  if (l->family != L_AF_INET) return -1;
  memset(out, 0, sizeof *out);
  out->sin_len = sizeof *out;
  out->sin_family = AF_INET;
  out->sin_port = l->port;
  out->sin_addr.s_addr = l->addr;
  return 0;
}
static void to_linux(const struct sockaddr_in *b, void *la, unsigned *llen) {
  struct l_sockaddr_in l;
  memset(&l, 0, sizeof l);
  l.family = L_AF_INET; l.port = b->sin_port; l.addr = b->sin_addr.s_addr;
  unsigned n = *llen < sizeof l ? *llen : sizeof l;
  if (la && n) memcpy(la, &l, n);
  *llen = sizeof l;
}

/* ------------------------------------------------------------- the calls */
int wf_socket(int domain, int type, int protocol) {
  if (domain == L_AF_INET6) FAIL(EAFNOSUPPORT);
  if (domain != L_AF_INET) FAIL(EAFNOSUPPORT);
  int t = type & 0xff;                       /* strip SOCK_NONBLOCK / SOCK_CLOEXEC */
  int fd = socket(AF_INET, t, protocol);
  if (fd < 0) { debugPrintf("[net] socket(type=%d) failed errno=%d\n", t, errno); FAIL(errno); }
  mark_sock(fd, 1);
  if (type & 0x800) { int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK); }
  return fd;
}

int wf_connect(int fd, const void *la, unsigned llen) {
  struct sockaddr_in b;
  if (to_bsd(la, llen, &b) < 0) FAIL(EAFNOSUPPORT);
  int r = connect(fd, (struct sockaddr *)&b, sizeof b);
  int e = errno;
  {
    static int logged;
    if (logged < 8) { logged++;
      debugPrintf("[net] connect(fd=%d, %s:%u) -> %d%s\n", fd, inet_ntoa(b.sin_addr),
                  (unsigned)ntohs(b.sin_port), r, r < 0 ? (e == EINPROGRESS ? " (in progress)" : " FAILED") : "");
      if (r < 0 && e != EINPROGRESS) debugPrintf("[net]   errno=%d\n", e);
    }
  }
  if (r < 0) FAIL(e);
  return 0;
}

int wf_bind(int fd, const void *la, unsigned llen) {
  struct sockaddr_in b;
  if (to_bsd(la, llen, &b) < 0) FAIL(EAFNOSUPPORT);
  return ret_i(bind(fd, (struct sockaddr *)&b, sizeof b));
}
int wf_listen(int fd, int backlog) { return ret_i(listen(fd, backlog)); }

int wf_accept(int fd, void *la, unsigned *llen) {
  struct sockaddr_in b; socklen_t bl = sizeof b;
  int r = accept(fd, (struct sockaddr *)&b, &bl);
  if (r < 0) FAIL(errno);
  mark_sock(r, 1);
  if (la && llen) to_linux(&b, la, llen);
  return r;
}

static int flags_to_bsd(int lf) {
  int f = 0;
  if (lf & L_MSG_OOB)      f |= MSG_OOB;
  if (lf & L_MSG_PEEK)     f |= MSG_PEEK;
  if (lf & L_MSG_WAITALL)  f |= MSG_WAITALL;
  if (lf & L_MSG_DONTWAIT) f |= MSG_DONTWAIT;
  /* MSG_NOSIGNAL: there is no SIGPIPE here; dropped */
  return f;
}
long wf_send(int fd, const void *buf, size_t len, int lf) { return ret_l(send(fd, buf, len, flags_to_bsd(lf))); }
long wf_recv(int fd, void *buf, size_t len, int lf)       { return ret_l(recv(fd, buf, len, flags_to_bsd(lf))); }
long wf_sendto(int fd, const void *buf, size_t len, int lf, const void *la, unsigned llen) {
  if (!la) return wf_send(fd, buf, len, lf);
  struct sockaddr_in b;
  if (to_bsd(la, llen, &b) < 0) FAIL(EAFNOSUPPORT);
  return ret_l(sendto(fd, buf, len, flags_to_bsd(lf), (struct sockaddr *)&b, sizeof b));
}
long wf_recvfrom(int fd, void *buf, size_t len, int lf, void *la, unsigned *llen) {
  struct sockaddr_in b; socklen_t bl = sizeof b;
  long r = recvfrom(fd, buf, len, flags_to_bsd(lf), la ? (struct sockaddr *)&b : NULL, la ? &bl : NULL);
  if (r < 0) FAIL(errno);
  if (la && llen) to_linux(&b, la, llen);
  return r;
}

/* struct msghdr / iovec: same layout on both (pointers + size_t); only the
 * address inside differs. The game uses these for plain stream IO. */
struct l_iovec { void *base; size_t len; };
struct l_msghdr { void *name; uint32_t namelen; struct l_iovec *iov; size_t iovlen; void *control; size_t controllen; int flags; };
long wf_sendmsg(int fd, const struct l_msghdr *m, int lf) {
  if (!m) FAIL(EINVAL);
  long total = 0;
  for (size_t i = 0; i < m->iovlen; i++) {
    long r = m->name ? wf_sendto(fd, m->iov[i].base, m->iov[i].len, lf, m->name, m->namelen)
                     : wf_send(fd, m->iov[i].base, m->iov[i].len, lf);
    if (r < 0) return total ? total : r;
    total += r;
    if ((size_t)r < m->iov[i].len) break;
  }
  return total;
}
long wf_recvmsg(int fd, struct l_msghdr *m, int lf) {
  if (!m || m->iovlen == 0) FAIL(EINVAL);
  unsigned nl = m->namelen;
  long r = wf_recvfrom(fd, m->iov[0].base, m->iov[0].len, lf, m->name, m->name ? &nl : NULL);
  if (r >= 0 && m->name) m->namelen = nl;
  m->flags = 0;
  return r;
}

int wf_shutdown(int fd, int how) { return ret_i(shutdown(fd, how)); }

static int so_to_bsd(int lv, int on, int *blv, int *bon) {
  if (lv == L_SOL_SOCKET) {
    *blv = SOL_SOCKET;
    switch (on) {
      case L_SO_REUSEADDR: *bon = SO_REUSEADDR; return 0;
      case L_SO_REUSEPORT: *bon = SO_REUSEADDR; return 0;
      case L_SO_TYPE:      *bon = SO_TYPE;      return 0;
      case L_SO_ERROR:     *bon = SO_ERROR;     return 0;
      case L_SO_BROADCAST: *bon = SO_BROADCAST; return 0;
      case L_SO_SNDBUF:    *bon = SO_SNDBUF;    return 0;
      case L_SO_RCVBUF:    *bon = SO_RCVBUF;    return 0;
      case L_SO_KEEPALIVE: *bon = SO_KEEPALIVE; return 0;
      case L_SO_LINGER:    *bon = SO_LINGER;    return 0;
      case L_SO_RCVTIMEO:  *bon = SO_RCVTIMEO;  return 0;
      case L_SO_SNDTIMEO:  *bon = SO_SNDTIMEO;  return 0;
      default: return -1;
    }
  }
  if (lv == IPPROTO_TCP) {
    *blv = IPPROTO_TCP;
    switch (on) {
      case TCP_NODELAY:     *bon = TCP_NODELAY;   return 0;
      case L_TCP_KEEPIDLE:  *bon = TCP_KEEPIDLE;  return 0;
      case L_TCP_KEEPINTVL: *bon = TCP_KEEPINTVL; return 0;
      case L_TCP_KEEPCNT:   *bon = TCP_KEEPCNT;   return 0;
      default: return -1;
    }
  }
  if (lv == L_IPPROTO_IPV6) return -1;
  if (lv == IPPROTO_IP) { *blv = IPPROTO_IP; *bon = on; return 0; }
  return -1;
}
int wf_setsockopt(int fd, int lv, int on, const void *v, unsigned l) {
  int blv, bon;
  if (so_to_bsd(lv, on, &blv, &bon) < 0) return 0;   /* unknown option: accept silently */
  int r = setsockopt(fd, blv, bon, v, l);
  if (r < 0) { static int n; if (n++ < 6) debugPrintf("[net] setsockopt(fd=%d, lv=%d, opt=%d) errno=%d (ignored)\n", fd, lv, on, errno); }
  return 0;   /* the engine treats a failed option as fatal; none of these are */
}
int wf_getsockopt(int fd, int lv, int on, void *v, unsigned *l) {
  int blv, bon;
  if (so_to_bsd(lv, on, &blv, &bon) < 0) FAIL(ENOPROTOOPT);
  socklen_t bl = l ? *l : 0;
  int r = getsockopt(fd, blv, bon, v, &bl);
  if (r < 0) FAIL(errno);
  if (l) *l = bl;
  if (lv == L_SOL_SOCKET && on == L_SO_ERROR && v && bl >= 4) *(int *)v = lerrno(*(int *)v);
  return 0;
}
int wf_getsockname(int fd, void *la, unsigned *llen) {
  struct sockaddr_in b; socklen_t bl = sizeof b;
  if (getsockname(fd, (struct sockaddr *)&b, &bl) < 0) FAIL(errno);
  to_linux(&b, la, llen);
  return 0;
}
int wf_getpeername(int fd, void *la, unsigned *llen) {
  struct sockaddr_in b; socklen_t bl = sizeof b;
  if (getpeername(fd, (struct sockaddr *)&b, &bl) < 0) FAIL(errno);
  to_linux(&b, la, llen);
  return 0;
}

/* fcntl / ioctl / poll / select for socket descriptors. Non-socket fds go back
 * to the substrate's own handlers (declared here, defined in imports.c). */
int wf_fcntl(int fd, int cmd, long arg) {
  if (cmd == L_F_GETFL) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) FAIL(errno);
    return (fl & O_NONBLOCK) ? L_O_NONBLOCK : 0;
  }
  if (cmd == L_F_SETFL) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) fl = 0;
    fl = (arg & L_O_NONBLOCK) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return ret_i(fcntl(fd, F_SETFL, fl));
  }
  return 0;   /* F_GETFD / F_SETFD (close-on-exec): nothing to do */
}
int wf_ioctl(int fd, unsigned long req, void *arg) {
  if (req == L_FIONBIO && arg) return wf_fcntl(fd, L_F_SETFL, *(int *)arg ? L_O_NONBLOCK : 0);
  FAIL(EINVAL);
}
int wf_poll(struct l_pollfd *fds, unsigned long nfds, int timeout) {
  /* pollfd and the POLL* bits are identical; only socket fds are pollable here.
   * Entries that are not sockets are reported as POLLNVAL, as poll(2) would for
   * a bad descriptor, which the engine handles. */
  if (!fds || nfds == 0) { if (timeout > 0) svcSleepThread((int64_t)timeout * 1000000); return 0; }
  struct pollfd *p = calloc(nfds, sizeof *p);
  if (!p) FAIL(ENOMEM);
  int nvalid = 0;
  for (unsigned long i = 0; i < nfds; i++) {
    p[i].fd = is_sock(fds[i].fd) ? fds[i].fd : -1;
    p[i].events = fds[i].events;
    if (p[i].fd >= 0) nvalid++;
  }
  int r;
  if (nvalid) r = poll(p, nfds, timeout);
  else { if (timeout > 0) svcSleepThread((int64_t)timeout * 1000000); r = 0; }
  int e = errno;
  int n = 0;
  for (unsigned long i = 0; i < nfds; i++) {
    fds[i].revents = (p[i].fd >= 0) ? p[i].revents : (fds[i].fd >= 0 ? POLLNVAL : 0);
    if (fds[i].revents) n++;
  }
  free(p);
  if (r < 0) FAIL(e);
  return n;
}

/* ------------------------------------------------------------ resolver */
int wf_getaddrinfo(const char *node, const char *svc, const struct l_addrinfo *hints, struct l_addrinfo **res) {
  if (res) *res = NULL;
  if (!node && !svc) return L_EAI_NONAME;
  if (hints && hints->ai_family == L_AF_INET6) return L_EAI_FAMILY;
  struct addrinfo bh, *bres = NULL;
  memset(&bh, 0, sizeof bh);
  bh.ai_family = AF_INET;
  if (hints) {
    bh.ai_socktype = hints->ai_socktype;
    bh.ai_protocol = hints->ai_protocol;
    if (hints->ai_flags & L_AI_PASSIVE)     bh.ai_flags |= AI_PASSIVE;
    if (hints->ai_flags & L_AI_NUMERICHOST) bh.ai_flags |= AI_NUMERICHOST;
  }
  int r = getaddrinfo(node, svc, &bh, &bres);
  if (r != 0) {
    debugPrintf("[net] getaddrinfo(%s) failed: %d (%s)\n", node ? node : "(null)", r, gai_strerror(r));
    switch (r) {
      case EAI_NONAME: case EAI_NODATA: return L_EAI_NONAME;
      case EAI_AGAIN:  return L_EAI_AGAIN;
      case EAI_MEMORY: return L_EAI_MEMORY;
      case EAI_FAMILY: return L_EAI_FAMILY;
      case EAI_SYSTEM: return L_EAI_SYSTEM;
      default:         return L_EAI_FAIL;
    }
  }
  struct l_addrinfo *head = NULL, **tail = &head;
  int n = 0;
  for (struct addrinfo *a = bres; a; a = a->ai_next) {
    if (a->ai_family != AF_INET || !a->ai_addr) continue;
    struct l_addrinfo *l = calloc(1, sizeof *l + sizeof(struct l_sockaddr_in));
    if (!l) break;
    l->ai_addr = (struct l_sockaddr_in *)(l + 1);
    to_linux((const struct sockaddr_in *)a->ai_addr, l->ai_addr, &(unsigned){ sizeof(struct l_sockaddr_in) });
    l->ai_addrlen = sizeof(struct l_sockaddr_in);
    l->ai_family = L_AF_INET;
    l->ai_socktype = a->ai_socktype ? a->ai_socktype : (hints ? hints->ai_socktype : SOCK_STREAM);
    l->ai_protocol = a->ai_protocol;
    if ((hints && (hints->ai_flags & L_AI_CANONNAME)) && a->ai_canonname) l->ai_canonname = strdup(a->ai_canonname);
    *tail = l; tail = &l->ai_next;
    if (n++ == 0) debugPrintf("[net] getaddrinfo(%s) -> %s\n", node ? node : "(null)",
                               inet_ntoa(((struct sockaddr_in *)a->ai_addr)->sin_addr));
  }
  freeaddrinfo(bres);
  if (!head) return L_EAI_NONAME;
  *res = head;
  return 0;
}
void wf_freeaddrinfo(struct l_addrinfo *l) {
  while (l) { struct l_addrinfo *n = l->ai_next; free(l->ai_canonname); free(l); l = n; }
}
int wf_getnameinfo(const void *la, unsigned llen, char *host, unsigned hl, char *serv, unsigned sl, int flags) {
  (void)flags;
  struct sockaddr_in b;
  if (to_bsd(la, llen, &b) < 0) return L_EAI_FAMILY;
  if (host && hl) snprintf(host, hl, "%s", inet_ntoa(b.sin_addr));   /* numeric only */
  if (serv && sl) snprintf(serv, sl, "%u", (unsigned)ntohs(b.sin_port));
  return 0;
}
uint32_t wf_inet_addr(const char *s) { return s ? inet_addr(s) : 0xffffffffu; }
int wf_inet_aton(const char *s, uint32_t *out) {
  if (!s) return 0;
  uint32_t a = inet_addr(s);
  if (a == 0xffffffffu && strcmp(s, "255.255.255.255")) return 0;
  if (out) *out = a;
  return 1;
}
int wf_inet_pton(int af, const char *s, void *dst) {
  if (af != L_AF_INET) { errno = L_EAFNOSUPPORT; return -1; }
  return wf_inet_aton(s, dst) ? 1 : 0;
}
const char *wf_inet_ntop(int af, const void *src, char *dst, unsigned size) {
  if (af != L_AF_INET || !src || !dst) { errno = L_EAFNOSUPPORT; return NULL; }
  struct in_addr a; memcpy(&a, src, 4);
  snprintf(dst, size, "%s", inet_ntoa(a));
  return dst;
}
char *wf_inet_ntoa(uint32_t in) {
  static char buf[20];
  struct in_addr a; a.s_addr = in;
  snprintf(buf, sizeof buf, "%s", inet_ntoa(a));
  return buf;
}

/* gethostbyname: Linux-layout hostent over an IPv4 answer. Rarely used by the
 * engine (getaddrinfo is the main path) but it is imported. */
struct l_hostent { char *name; char **aliases; int addrtype; int length; char **addr_list; };
void *wf_gethostbyname(const char *name) {
  static struct l_hostent he; static char *aliases[1]; static uint32_t addr; static char *list[2]; static char hname[256];
  struct l_addrinfo *r = NULL;
  if (wf_getaddrinfo(name, NULL, NULL, &r) != 0 || !r) return NULL;
  addr = r->ai_addr->addr;
  wf_freeaddrinfo(r);
  snprintf(hname, sizeof hname, "%s", name);
  aliases[0] = NULL; list[0] = (char *)&addr; list[1] = NULL;
  he.name = hname; he.aliases = aliases; he.addrtype = L_AF_INET; he.length = 4; he.addr_list = list;
  return &he;
}

/* ---------------------------------------------------------------- table */
DynLibFunction wf_net_functions[] = {
  { "socket",      (uintptr_t)&wf_socket },      { "connect",     (uintptr_t)&wf_connect },
  { "bind",        (uintptr_t)&wf_bind },        { "listen",      (uintptr_t)&wf_listen },
  { "accept",      (uintptr_t)&wf_accept },      { "send",        (uintptr_t)&wf_send },
  { "recv",        (uintptr_t)&wf_recv },        { "sendto",      (uintptr_t)&wf_sendto },
  { "recvfrom",    (uintptr_t)&wf_recvfrom },    { "sendmsg",     (uintptr_t)&wf_sendmsg },
  { "recvmsg",     (uintptr_t)&wf_recvmsg },     { "shutdown",    (uintptr_t)&wf_shutdown },
  { "setsockopt",  (uintptr_t)&wf_setsockopt },  { "getsockopt",  (uintptr_t)&wf_getsockopt },
  { "getsockname", (uintptr_t)&wf_getsockname }, { "getpeername", (uintptr_t)&wf_getpeername },
  { "getaddrinfo", (uintptr_t)&wf_getaddrinfo }, { "freeaddrinfo",(uintptr_t)&wf_freeaddrinfo },
  { "getnameinfo", (uintptr_t)&wf_getnameinfo }, { "gethostbyname",(uintptr_t)&wf_gethostbyname },
  { "inet_addr",   (uintptr_t)&wf_inet_addr },   { "inet_aton",   (uintptr_t)&wf_inet_aton },
  { "inet_pton",   (uintptr_t)&wf_inet_pton },   { "inet_ntop",   (uintptr_t)&wf_inet_ntop },
  { "inet_ntoa",   (uintptr_t)&wf_inet_ntoa },
};
size_t wf_net_numfunctions = sizeof(wf_net_functions) / sizeof(wf_net_functions[0]);
