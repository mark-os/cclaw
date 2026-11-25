#define _GNU_SOURCE
/* The connect() supervisor (specs/containment.md, NE_NOTIF and transparent
 * interception): a thread in the --run-tool broker — outside every namespace,
 * Landlock domain and filter, and an ancestor of every contained process —
 * that answers the shell child's seccomp user notifications. It takes the
 * socket the call names (pidfd_getfd), reads the address once
 * (process_vm_readv), and connects that socket itself: to the shim's loopback
 * port with the shim's own HTTP CONNECT, so the proxy decides as for any
 * other connection. The caller gets back a real TCP socket. When interception
 * is the enforcer (no network namespace) no call is ever continued: the
 * kernel would re-read arguments the caller can change after the check.
 * Also the DNS stub the namespace's resolv.conf points at. */
#include "contain.h"
#include "log.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

/* The user-notification ABI, defined here as contain_linux.c defines the
 * filter's: the static builds have no <linux/seccomp.h>. */
struct sn_data { int32_t nr; uint32_t arch; uint64_t ip; uint64_t args[6]; };
struct sn_notif { uint64_t id; uint32_t pid; uint32_t flags; struct sn_data data; };
struct sn_resp { uint64_t id; int64_t val; int32_t error; uint32_t flags; };
#define SN_RECV     _IOWR('!', 0, struct sn_notif)
#define SN_SEND     _IOWR('!', 1, struct sn_resp)
#define SN_ID_VALID _IOW('!', 2, uint64_t)
#define SN_CONTINUE 1u
#ifndef __NR_pidfd_open
#define __NR_pidfd_open  434
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif

#define DNS_BASE 0xC6120000u   /* 198.18.0.0/15: benchmark space, never routed */

struct ContainSup {
    pthread_t th;
    pthread_attr_t detached;
    pthread_mutex_t mu;        /* names: the DNS thread adds, call threads read */
    int sock, listener, dns, port, stop[2];
    int enforce;               /* NE_NOTIF: nothing is continued */
    int abstract_ok;           /* files 'open': host abstract sockets are in reach */
    ContainDnsNames *names;
};

typedef struct { ContainSup *s; struct sn_notif n; } Call;

/* ── DNS stub ───────────────────────────────────────────────────────────── */

/* A query for an A record gets the name's own address from 198.18.0.0/15,
 * AAAA an empty answer, anything else NOTIMP. No lookup: the proxy resolves
 * the real name when the connection is made. A name is kept only in the
 * characters a hostname has — it travels on in a CONNECT line. */
size_t contain_dns_reply(ContainDnsNames *t, const unsigned char *q, size_t qn,
                         unsigned char *out, size_t cap) {
    if (qn < 12 || cap < 512 || (q[2] & 0x80)) return 0;  /* no id, or a reply */
    char name[254];
    size_t i = 12, nl = 0;
    int rcode = 0, bad = 0, qok = 0;
    if ((q[2] >> 3) & 0xf) rcode = 4;                     /* not a standard query */
    else if (q[4] != 0 || q[5] != 1) rcode = 1;           /* exactly one question */
    else {
        for (;;) {
            if (i >= qn) { rcode = 1; break; }
            unsigned l = q[i++];
            if (l == 0) break;
            /* > 63 is also a compression pointer: none in a question. */
            if (l > 63 || i + l > qn || nl + l + 1 > sizeof(name) - 1) { rcode = 1; break; }
            if (nl) name[nl++] = '.';
            for (unsigned j = 0; j < l; j++) {
                unsigned char c = q[i + j];
                if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
                bad |= !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                         c == '-' || c == '_');
                name[nl++] = (char)c;
            }
            i += l;
        }
        if (!rcode && i + 4 > qn) rcode = 1;
        qok = !rcode;
    }
    name[nl] = '\0';
    unsigned qtype = qok ? (unsigned)(q[i] << 8 | q[i + 1]) : 0;
    if (qok && (q[i + 2] != 0 || q[i + 3] != 1)) rcode = 4;   /* class IN only */
    else if (qok && qtype != 1 && qtype != 28) rcode = 4;
    else if (qok && (bad || nl == 0)) rcode = 3;
    uint32_t addr = 0;
    if (qok && !rcode && qtype == 1) {
        size_t k = 0;
        while (k < t->n && strcmp(t->name[k], name) != 0) k++;
        if (k == t->n && k < CONTAIN_DNS_NAMES) memcpy(t->name[t->n++], name, nl + 1);
        if (k < t->n) addr = DNS_BASE + (uint32_t)k + 1;
        else rcode = 2;                                          /* table full */
    }
    size_t len = qok ? i + 4 : 12;
    memcpy(out, q, len);
    out[2] = (unsigned char)(0x80 | (q[2] & 0x01));   /* a reply; RD echoed */
    out[3] = (unsigned char)(0x80 | rcode);           /* RA */
    memset(out + 4, 0, 8);
    out[5] = (unsigned char)qok;
    if (addr) {
        static const unsigned char rr[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
        out[7] = 1;
        memcpy(out + len, rr, sizeof(rr));
        len += sizeof(rr);
        uint32_t be = htonl(addr);
        memcpy(out + len, &be, 4);
        len += 4;
    }
    return len;
}

static void dns_serve(ContainSup *s) {
    unsigned char q[512], a[512];
    struct sockaddr_storage from;
    socklen_t fl = sizeof(from);
    ssize_t n = recvfrom(s->dns, q, sizeof(q), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n <= 0) return;
    pthread_mutex_lock(&s->mu);
    size_t r = contain_dns_reply(s->names, q, (size_t)n, a, sizeof(a));
    pthread_mutex_unlock(&s->mu);
    if (r) (void)!sendto(s->dns, a, r, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&from, fl);
}

/* ── One connect() ──────────────────────────────────────────────────────── */

/* Wait for ev on fd a second at a time, for as long as the caller is still
 * in its connect(): a killed caller ends the wait, nothing else does. */
static int wait_io(ContainSup *s, uint64_t id, int fd, short ev) {
    for (;;) {
        struct pollfd p = { fd, ev, 0 };
        int r = poll(&p, 1, 1000);
        if (r > 0) return 0;
        if (r < 0 && errno != EINTR) return -1;
        if (ioctl(s->listener, SN_ID_VALID, &id) != 0) return -1;
    }
}

static int loopback(const struct sockaddr_storage *ss) {
    if (ss->ss_family == AF_INET)
        return (ntohl(((const struct sockaddr_in *)ss)->sin_addr.s_addr) >> 24) == 127;
    const struct in6_addr *a = &((const struct sockaddr_in6 *)ss)->sin6_addr;
    return IN6_IS_ADDR_LOOPBACK(a) || (IN6_IS_ADDR_V4MAPPED(a) && a->s6_addr[12] == 127);
}

/* "host:port" for the CONNECT line: the name the stub handed this address out
 * for, or the literal address. */
static void host_port(ContainSup *s, const struct sockaddr_storage *ss, char *out, size_t cap) {
    char ip[INET6_ADDRSTRLEN] = "";
    unsigned port;
    uint32_t v4;
    const struct in6_addr *a6 = &((const struct sockaddr_in6 *)ss)->sin6_addr;
    if (ss->ss_family == AF_INET) {
        v4 = ntohl(((const struct sockaddr_in *)ss)->sin_addr.s_addr);
        port = ntohs(((const struct sockaddr_in *)ss)->sin_port);
    } else {
        port = ntohs(((const struct sockaddr_in6 *)ss)->sin6_port);
        if (!IN6_IS_ADDR_V4MAPPED(a6)) {
            inet_ntop(AF_INET6, a6, ip, sizeof(ip));
            snprintf(out, cap, "[%s]:%u", ip, port);
            return;
        }
        memcpy(&v4, &a6->s6_addr[12], 4);
        v4 = ntohl(v4);
    }
    if ((v4 & 0xfffe0000u) == DNS_BASE && s->names) {
        pthread_mutex_lock(&s->mu);
        uint32_t k = v4 - DNS_BASE - 1;
        int named = k < s->names->n;
        if (named) snprintf(out, cap, "%s:%u", s->names->name[k], port);
        pthread_mutex_unlock(&s->mu);
        if (named) return;
    }
    struct in_addr in = { htonl(v4) };
    inet_ntop(AF_INET, &in, ip, sizeof(ip));
    snprintf(out, cap, "%s:%u", ip, port);
}

/* Connect S to the shim, ask it for hostport, and swallow its reply's head —
 * a byte at a time, so whatever the destination sends first stays in the
 * socket for the caller. 0, or ECONNREFUSED with S disconnected again. */
static int tunnel(ContainSup *s, uint64_t id, int S, int dom, const char *hostport) {
    if (s->port <= 0) return ECONNREFUSED;
    struct sockaddr_in a4 = { .sin_family = AF_INET, .sin_port = htons((uint16_t)s->port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    struct sockaddr_in6 a6 = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t)s->port) };
    a6.sin6_addr.s6_addr[10] = a6.sin6_addr.s6_addr[11] = 0xff;   /* ::ffff:127.0.0.1 */
    a6.sin6_addr.s6_addr[12] = 127;
    a6.sin6_addr.s6_addr[15] = 1;
    int r = dom == AF_INET ? connect(S, (struct sockaddr *)&a4, sizeof(a4))
                           : connect(S, (struct sockaddr *)&a6, sizeof(a6));
    if (r != 0) {
        int e = 0;
        socklen_t el = sizeof(e);
        if (errno == EISCONN || errno == EALREADY) return errno;   /* as the kernel says */
        if (errno != EINPROGRESS || wait_io(s, id, S, POLLOUT) != 0 ||
            getsockopt(S, SOL_SOCKET, SO_ERROR, &e, &el) != 0 || e != 0)
            goto refuse;
    }
    char req[640], head[512];
    int n = snprintf(req, sizeof(req), "CONNECT %s HTTP/1.1\r\nHost: %s\r\n\r\n", hostport, hostport);
    for (int off = 0; off < n; ) {
        ssize_t w = send(S, req + off, (size_t)(n - off), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w > 0) off += (int)w;
        else if (w < 0 && (errno == EAGAIN || errno == EINTR) &&
                 wait_io(s, id, S, POLLOUT) == 0) continue;
        else goto refuse;
    }
    size_t h = 0;
    while (h < 4 || memcmp(head + h - 4, "\r\n\r\n", 4) != 0) {
        if (h == sizeof(head)) goto refuse;
        ssize_t got = recv(S, head + h, 1, MSG_DONTWAIT);
        if (got == 1) h++;
        else if (got < 0 && (errno == EAGAIN || errno == EINTR) &&
                 wait_io(s, id, S, POLLIN) == 0) continue;
        else goto refuse;
    }
    if (h > 12 && memcmp(head, "HTTP/1.", 7) == 0 && memcmp(head + 8, " 200", 4) == 0)
        return 0;
refuse:;
    struct sockaddr un = { .sa_family = AF_UNSPEC };
    connect(S, &un, sizeof(un));
    return ECONNREFUSED;
}

/* NE_NOTIF, AF_UNIX: the supervisor connects S itself, a relative path
 * resolved against the caller's working directory. The path is resolved as
 * the caller would (same user, same mount namespace; Landlock does not
 * mediate a pathname socket's connect). An abstract address is the host's
 * namespace-wide one, which Landlock's scoping would refuse: only under files
 * 'open'. */
static int unix_connect(ContainSup *s, int S, const struct sockaddr_storage *ss,
                        socklen_t len, pid_t pid) {
    const struct sockaddr_un *u = (const struct sockaddr_un *)ss;
    size_t off = offsetof(struct sockaddr_un, sun_path);
    if (len <= off) return EINVAL;
    if (u->sun_path[0] == '\0' && !s->abstract_ok) return EPERM;
    if (u->sun_path[0] == '\0' || u->sun_path[0] == '/')
        return connect(S, (const struct sockaddr *)u, len) == 0 ? 0 : errno;
    char dir[64];
    snprintf(dir, sizeof(dir), "/proc/%d/cwd", (int)pid);
    int dfd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return EPERM;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    int rc = ENAMETOOLONG;
    if (snprintf(a.sun_path, sizeof(a.sun_path), "/proc/self/fd/%d/%.*s", dfd,
                 (int)strnlen(u->sun_path, len - off), u->sun_path) < (int)sizeof(a.sun_path))
        rc = connect(S, (struct sockaddr *)&a, sizeof(a)) == 0 ? 0 : errno;
    close(dfd);
    return rc;
}

/* Decide on the socket object held, never on a second read of anything the
 * caller owns. An inet stream to anywhere but loopback is tunnelled; the
 * rest is continued in the namespace, or performed here when there is none. */
static int decide(ContainSup *s, uint64_t id, int S, const struct sockaddr_storage *ss,
                  socklen_t len, pid_t pid, int *cont) {
    int dom = -1, type = -1;
    socklen_t ol = sizeof(int);
    if (getsockopt(S, SOL_SOCKET, SO_DOMAIN, &dom, &ol) != 0)
        return errno == ENOTSOCK ? ENOTSOCK : EPERM;
    ol = sizeof(int);
    if (getsockopt(S, SOL_SOCKET, SO_TYPE, &type, &ol) != 0) return EPERM;
    int fam = len >= sizeof(sa_family_t) ? ss->ss_family : -1;
    int inet = type == SOCK_STREAM &&
        ((dom == AF_INET && fam == AF_INET && len >= sizeof(struct sockaddr_in)) ||
         (dom == AF_INET6 && fam == AF_INET6 && len >= sizeof(struct sockaddr_in6)));
    if (inet && !loopback(ss)) {
        char hp[300];
        host_port(s, ss, hp, sizeof(hp));
        return tunnel(s, id, S, dom, hp);
    }
    if (!s->enforce) { *cont = 1; return 0; }
    if (inet || fam == AF_UNSPEC || (dom == AF_NETLINK && fam == AF_NETLINK))
        return connect(S, (const struct sockaddr *)ss, len) == 0 ? 0 : errno;
    if (dom == AF_UNIX && fam == AF_UNIX) return unix_connect(s, S, ss, len, pid);
    return EPERM;
}

/* The notification names the calling thread; pidfd_open wants its process.
 * A reused id is caught by NOTIF_ID_VALID after the open. */
static pid_t tgid_of(pid_t tid) {
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)tid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    const char *t = strstr(buf, "\nTgid:");
    return t ? (pid_t)atoi(t + 6) : -1;
}

/* pidfd_open pins the process, NOTIF_ID_VALID then proves it is still the
 * caller (not a reused pid), pidfd_getfd takes the socket object the call
 * names; the address is read once and the call re-validated after. Any
 * failure here is EPERM: a caller we cannot inspect does not connect. */
static int take_and_connect(ContainSup *s, const struct sn_notif *n, int *cont) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    uint64_t id = n->id, alen = n->data.args[2] & 0xffffffffu;
    if (alen > sizeof(ss)) return EINVAL;
    int pfd = (int)syscall(__NR_pidfd_open, tgid_of((pid_t)n->pid), 0);
    if (pfd < 0) return EPERM;
    int S = ioctl(s->listener, SN_ID_VALID, &id) == 0
            ? (int)syscall(__NR_pidfd_getfd, pfd, (int)n->data.args[0], 0) : -1;
    int e = errno;
    close(pfd);
    if (S < 0) return e == EBADF ? EBADF : EPERM;
    struct iovec mine = { &ss, (size_t)alen };
    struct iovec theirs = { (void *)(uintptr_t)n->data.args[1], (size_t)alen };
    int rc = EPERM;
    if ((alen == 0 || process_vm_readv((pid_t)n->pid, &mine, 1, &theirs, 1, 0) == (ssize_t)alen) &&
        ioctl(s->listener, SN_ID_VALID, &id) == 0)
        rc = decide(s, id, S, &ss, (socklen_t)alen, (pid_t)n->pid, cont);
    close(S);
    return rc;
}

static void answer(ContainSup *s, uint64_t id, int err, int cont) {
    struct sn_resp r = { .id = id, .error = cont ? 0 : -err,
                         .flags = cont ? SN_CONTINUE : 0 };
    ioctl(s->listener, SN_SEND, &r);   /* ENOENT: the caller is gone */
}

/* One thread per call: a destination that is slow to answer holds up its
 * own caller and nobody else. */
static void *call_thread(void *arg) {
    Call *c = arg;
    int cont = 0;
    int err = take_and_connect(c->s, &c->n, &cont);
    answer(c->s, c->n.id, err, cont);
    free(c);
    return NULL;
}

/* ── The supervisor ─────────────────────────────────────────────────────── */

/* The child's hand-off: the listener, the DNS socket when it has one, and
 * the shim's port. Returns 0, or -1 when the child never sent it. */
static int recv_handoff(ContainSup *s) {
    struct pollfd p[2] = { { s->sock, POLLIN, 0 }, { s->stop[0], POLLIN, 0 } };
    while (poll(p, 2, -1) < 0)
        if (errno != EINTR) return -1;
    if (p[1].revents) return -1;
    int32_t port = 0;
    int fds[2] = { -1, -1 };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(fds))]; } c;
    memset(&c, 0, sizeof(c));
    struct iovec io = { &port, sizeof(port) };
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = c.b,
                        .msg_controllen = sizeof(c.b) };
    ssize_t n = recvmsg(s->sock, &m, MSG_CMSG_CLOEXEC);
    struct cmsghdr *h = n == (ssize_t)sizeof(port) ? CMSG_FIRSTHDR(&m) : NULL;
    size_t nfd = h && h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS
                 ? (h->cmsg_len - CMSG_LEN(0)) / sizeof(int) : 0;
    if (nfd < 1 || nfd > 2) return -1;
    memcpy(fds, CMSG_DATA(h), nfd * sizeof(int));
    s->listener = fds[0];
    s->dns = fds[1];
    s->port = port;
    if (s->dns >= 0 && !(s->names = calloc(1, sizeof(*s->names)))) {
        close(s->dns);
        s->dns = -1;
    }
    return 0;
}

static void *sup_main(void *arg) {
    ContainSup *s = arg;
    if (recv_handoff(s) != 0) return NULL;
    for (;;) {
        struct pollfd p[3] = { { s->stop[0], POLLIN, 0 }, { s->listener, POLLIN, 0 },
                               { s->dns, POLLIN, 0 } };
        if (poll(p, s->dns >= 0 ? 3 : 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (p[0].revents) break;
        if (s->dns >= 0 && (p[2].revents & POLLIN)) dns_serve(s);
        if (p[1].revents & POLLIN) {
            Call *c = calloc(1, sizeof(*c));
            struct sn_notif spare;
            memset(&spare, 0, sizeof(spare));
            if (ioctl(s->listener, SN_RECV, c ? &c->n : &spare) != 0) {
                free(c);
                if (errno == EINTR || errno == ENOENT) continue;   /* caller gone */
                break;
            }
            pthread_t t;
            if (c) c->s = s;
            if (!c || pthread_create(&t, &s->detached, call_thread, c) != 0) {
                answer(s, c ? c->n.id : spare.id, EPERM, 0);
                free(c);
            }
        } else if (p[1].revents & (POLLHUP | POLLERR)) {
            break;   /* no process is left under the filter */
        }
    }
    return NULL;
}

ContainSup *contain_supervise(int sock, const ContainPlan *p) {
    ContainSup *s = calloc(1, sizeof(*s));
    if (!s || pipe2(s->stop, O_CLOEXEC) != 0) {
        free(s);
        close(sock);
        return NULL;
    }
    s->sock = sock;
    s->listener = s->dns = -1;
    s->enforce = p->ne == NE_NOTIF;
    s->abstract_ok = p->lv.files == FILES_OPEN;
    pthread_mutex_init(&s->mu, NULL);
    pthread_attr_init(&s->detached);
    pthread_attr_setdetachstate(&s->detached, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&s->th, NULL, sup_main, s) != 0) {
        LOG_INFO_("sandbox supervisor_start_fail errno=%d", errno);
        close(sock);
        close(s->stop[0]);
        close(s->stop[1]);
        free(s);
        return NULL;
    }
    return s;
}

/* Not freed: a call thread may still be sending its answer, and the broker
 * exits right after this. */
void contain_supervise_end(ContainSup *s) {
    if (!s) return;
    if (write(s->stop[1], "x", 1) != 1) { /* the thread may already be gone */ }
    pthread_join(s->th, NULL);
}
