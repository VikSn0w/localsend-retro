/* POSIX backend (Mac OS X 10.5+): BSD sockets, pthreads, stdio-less file I/O. */
#define _DARWIN_C_SOURCE
#define _BSD_SOURCE
#define _DEFAULT_SOURCE
#include "ls_plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0
#endif

struct plat_conn { int fd; };
struct plat_listener { int fd; };
struct plat_udp { int fd; };
struct plat_file { int fd; };

void plat_init(void)
{
    signal(SIGPIPE, SIG_IGN);
}

double plat_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

unsigned long plat_rand32(void)
{
    unsigned long v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) v = 0;
        close(fd);
    }
    if (v == 0) {                        /* fallback: not secure, only for session ids */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        v = (unsigned long)(tv.tv_usec * 2654435761UL) ^ (unsigned long)tv.tv_sec ^ (unsigned long)getpid();
    }
    return v;
}

/* dotted quad helpers (inet_pton/inet_ntop are missing on Mac OS X before 10.3) */
static int ip_parse(const char *s, struct in_addr *out)
{
    return inet_aton(s, out) ? 0 : -1;
}

static void ip_format(struct in_addr a, char *out)
{
    unsigned long h = ntohl(a.s_addr);
    sprintf(out, "%lu.%lu.%lu.%lu", (h >> 24) & 255, (h >> 16) & 255, (h >> 8) & 255, h & 255);
}

/* ---------------------------------------------------------------- threads */

void *plat_mutex_new(void)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof *m);
    pthread_mutex_init(m, NULL);
    return m;
}

void plat_mutex_lock(void *m) { pthread_mutex_lock((pthread_mutex_t *)m); }
void plat_mutex_unlock(void *m) { pthread_mutex_unlock((pthread_mutex_t *)m); }

typedef struct { plat_thread_fn fn; void *arg; } tstart;

static void *tramp(void *p)
{
    tstart t = *(tstart *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}

int plat_thread_start(plat_thread_fn fn, void *arg, void **handle)
{
    pthread_t *th = (pthread_t *)malloc(sizeof *th);
    pthread_attr_t at;
    tstart *t = (tstart *)malloc(sizeof *t);
    int rc;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_init(&at);
    if (!handle) pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(th, &at, tramp, t);
    pthread_attr_destroy(&at);
    if (rc != 0) { free(t); free(th); return -1; }
    if (handle) *handle = th;
    else free(th);
    return 0;
}

void plat_thread_join(void *handle)
{
    if (!handle) return;
    pthread_join(*(pthread_t *)handle, NULL);
    free(handle);
}

/* -------------------------------------------------------------------- UDP */

plat_udp *plat_udp_open(int port, const char *group)
{
    plat_udp *u = (plat_udp *)calloc(1, sizeof *u);
    struct sockaddr_in sa;
    struct ip_mreq mr;
    unsigned char ttl = 1;
    int on = 1;

    u->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (u->fd < 0) { free(u); return NULL; }
    setsockopt(u->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
    setsockopt(u->fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)port);
    if (bind(u->fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(u->fd);
        free(u);
        return NULL;
    }
    if (ip_parse(group, &mr.imr_multiaddr) != 0) { close(u->fd); free(u); return NULL; }
    mr.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(u->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr);
    setsockopt(u->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    return u;
}

int plat_udp_send(plat_udp *u, const char *ip, int port, const void *buf, size_t n)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (ip_parse(ip, &sa.sin_addr) != 0) return -1;
    return sendto(u->fd, buf, n, 0, (struct sockaddr *)&sa, sizeof sa) < 0 ? -1 : 0;
}

long plat_udp_recv(plat_udp *u, void *buf, size_t n, char *ip_out, int timeout_ms)
{
    fd_set rs;
    struct timeval tv;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    ssize_t r;

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    FD_ZERO(&rs);
    FD_SET(u->fd, &rs);
    if (select(u->fd + 1, &rs, NULL, NULL, &tv) <= 0) return 0;
    r = recvfrom(u->fd, buf, n, 0, (struct sockaddr *)&sa, &sl);
    if (r < 0) return -1;
    ip_format(sa.sin_addr, ip_out);
    return (long)r;
}

void plat_udp_close(plat_udp *u)
{
    if (!u) return;
    close(u->fd);
    free(u);
}

/* -------------------------------------------------------------------- TCP */

static void set_timeouts(int fd, int rcv, int snd)
{
    struct timeval tv;
    int one = 1;
    tv.tv_usec = 0;
    tv.tv_sec = rcv; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    tv.tv_sec = snd; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
}

static plat_conn *wrap(int fd)
{
    plat_conn *c = (plat_conn *)malloc(sizeof *c);
    int big = 256 * 1024;
    c->fd = fd;
    set_timeouts(fd, 600, 60);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    return c;
}

plat_listener *plat_tcp_listen(int port)
{
    plat_listener *l = (plat_listener *)calloc(1, sizeof *l);
    struct sockaddr_in sa;
    int on = 1;
    l->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (l->fd < 0) { free(l); return NULL; }
    setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)port);
    if (bind(l->fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(l->fd, 16) < 0) {
        close(l->fd);
        free(l);
        return NULL;
    }
    return l;
}

plat_conn *plat_tcp_accept(plat_listener *l, char *ip_out, int timeout_ms)
{
    fd_set rs;
    struct timeval tv;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd;

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    FD_ZERO(&rs);
    FD_SET(l->fd, &rs);
    if (select(l->fd + 1, &rs, NULL, NULL, &tv) <= 0) return NULL;
    fd = accept(l->fd, (struct sockaddr *)&sa, &sl);
    if (fd < 0) return NULL;
    ip_format(sa.sin_addr, ip_out);
    return wrap(fd);
}

void plat_listener_close(plat_listener *l)
{
    if (!l) return;
    close(l->fd);
    free(l);
}

plat_conn *plat_tcp_connect(const char *ip, int port, int timeout_s)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), fl, err = 0;
    socklen_t el = sizeof err;
    fd_set ws;
    struct timeval tv;

    if (fd < 0) return NULL;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (ip_parse(ip, &sa.sin_addr) != 0) { close(fd); return NULL; }
    fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        if (errno != EINPROGRESS) { close(fd); return NULL; }
        FD_ZERO(&ws);
        FD_SET(fd, &ws);
        tv.tv_sec = timeout_s;
        tv.tv_usec = 0;
        if (select(fd + 1, NULL, &ws, NULL, &tv) <= 0) { close(fd); return NULL; }
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(fd); return NULL; }
    }
    fcntl(fd, F_SETFL, fl);
    return wrap(fd);
}

long plat_conn_read(plat_conn *c, void *buf, size_t n)
{
    return (long)recv(c->fd, buf, n, 0);
}

long plat_conn_write(plat_conn *c, const void *buf, size_t n)
{
    return (long)send(c->fd, buf, n, 0);
}

int plat_conn_peek(plat_conn *c)
{
    unsigned char b = 0;
    if (recv(c->fd, &b, 1, MSG_PEEK) != 1) return -1;
    return b;
}

int plat_conn_fd(plat_conn *c) { return c->fd; }

void plat_conn_close(plat_conn *c)
{
    if (!c) return;
    close(c->fd);
    free(c);
}

/* ------------------------------------------------------------------ files */

void plat_mkdir(const char *dir) { mkdir(dir, 0755); }

plat_file *plat_file_create_unique(const char *dir, const char *name, char *path_out, size_t ps)
{
    char base[256], ext[64];
    char *dot;
    int n, fd;
    plat_file *f;

    strncpy(base, name, sizeof base - 1);
    base[sizeof base - 1] = 0;
    ext[0] = 0;
    dot = strrchr(base, '.');
    if (dot && dot != base && strlen(dot) < sizeof ext) {
        strcpy(ext, dot);
        *dot = 0;
    }
    for (n = 0; n < 1000; n++) {
        if (n == 0) snprintf(path_out, ps, "%s/%s%s", dir, base, ext);
        else snprintf(path_out, ps, "%s/%s (%d)%s", dir, base, n, ext);
        fd = open(path_out, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            f = (plat_file *)malloc(sizeof *f);
            f->fd = fd;
            return f;
        }
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}

long plat_file_write(plat_file *f, const void *buf, size_t n)
{
    return (long)write(f->fd, buf, n);
}

void plat_file_close(plat_file *f)
{
    if (!f) return;
    close(f->fd);
    free(f);
}

void plat_file_remove(const char *path) { unlink(path); }

plat_file *plat_file_open_read(const char *path)
{
    int fd = open(path, O_RDONLY);
    plat_file *f;
    if (fd < 0) return NULL;
    f = (plat_file *)malloc(sizeof *f);
    f->fd = fd;
    return f;
}

long plat_file_read(plat_file *f, void *buf, size_t n)
{
    return (long)read(f->fd, buf, n);
}

int plat_file_size(const char *path, long long *size)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    *size = (long long)st.st_size;
    return 0;
}

const char *plat_basename(const char *path)
{
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}
