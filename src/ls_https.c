#include "ls_https.h"
#include "ls_tls.h"
#include "ls_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0
#endif

static void set_err(char *err, size_t es, const char *msg)
{
    if (es) {
        strncpy(err, msg, es - 1);
        err[es - 1] = 0;
    }
}

/* splits https://host[:port]/path */
static int parse_url(const char *url, char *host, size_t hs, int *port, char *path, size_t ps)
{
    const char *p, *slash, *colon;
    size_t hl;
    if (strncmp(url, "https://", 8) != 0) return -1;
    p = url + 8;
    slash = strchr(p, '/');
    if (!slash) slash = p + strlen(p);
    colon = memchr(p, ':', (size_t)(slash - p));
    hl = (size_t)((colon ? colon : slash) - p);
    if (!hl || hl >= hs) return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    *port = colon ? atoi(colon + 1) : 443;
    if (*port <= 0 || *port > 65535) return -1;
    if (strlen(slash) >= ps) return -1;
    strcpy(path, *slash ? slash : "/");
    return 0;
}

static int connect_host(const char *host, int port, char *err, size_t es)
{
    struct hostent *he = gethostbyname(host);
    struct sockaddr_in sa;
    int fd, fl, e = 0;
    socklen_t el = sizeof e;
    fd_set ws;
    struct timeval tv;
    int one = 1;

    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) {
        set_err(err, es, "cannot find the server (no internet connection?)");
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
    fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        if (errno != EINPROGRESS) goto fail;
        FD_ZERO(&ws);
        FD_SET(fd, &ws);
        tv.tv_sec = 15;
        tv.tv_usec = 0;
        if (select(fd + 1, NULL, &ws, NULL, &tv) <= 0) goto fail;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
        if (e) goto fail;
    }
    fcntl(fd, F_SETFL, fl);
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    return fd;
fail:
    close(fd);
    set_err(err, es, "cannot connect to the server");
    return -1;
}

static void dechunk(char *body, size_t *len)
{
    char *in = body, *out = body, *end = body + *len;
    while (in < end) {
        unsigned long sz = (unsigned long)ls_hextoll(in);
        char *nl = strstr(in, "\r\n");
        if (!nl) break;
        in = nl + 2;
        if (sz == 0 || in + sz > end) break;
        memmove(out, in, sz);
        out += sz;
        in += sz + 2;
    }
    *out = 0;
    *len = (size_t)(out - body);
}

/* one request; on a redirect fills `loc` and returns 1, on a final answer returns 0 */
static int fetch_once(const char *url, const char *ca_file, long max_bytes, ls_dl_progress prog, void *ud,
                      char **body, long *len, int *status, char *loc, size_t ls, char *err, size_t es)
{
    char host[256], path[1024], req[1500];
    int port, fd, rc = -1, chunked = 0;
    ls_tls *t;
    char *buf = NULL;
    size_t have = 0, cap = 0, hend = 0;
    long long clen = -1;

    if (parse_url(url, host, sizeof host, &port, path, sizeof path) != 0) {
        set_err(err, es, "only https:// addresses are allowed");
        return -1;
    }
    fd = connect_host(host, port, err, es);
    if (fd < 0) return -1;
    err[0] = 0;
    t = ls_tls_connect_verified(fd, host, ca_file, err, es);
    if (!t) {
        close(fd);
        if (!err[0]) set_err(err, es, "secure connection failed");
        return -1;
    }
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: LocalSend-PPC-updater\r\n"
             "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (ls_tls_write(t, req, strlen(req)) < 0) { set_err(err, es, "request failed"); goto out; }

    for (;;) {
        int r;
        if (have + 8192 + 1 > cap) {
            cap = cap ? cap * 2 : 16384;
            if ((long)cap > max_bytes + 65536) cap = (size_t)max_bytes + 65536;
            if (have + 4096 + 1 > cap) { set_err(err, es, "download too large"); goto out; }
            buf = (char *)realloc(buf, cap);
        }
        r = ls_tls_read(t, buf + have, cap - have - 1);
        if (r < 0) { set_err(err, es, "connection lost during download"); goto out; }
        if (r == 0) break;
        have += (size_t)r;
        buf[have] = 0;
        if (!hend) {
            char *e = strstr(buf, "\r\n\r\n");
            if (e) {
                char *p;
                hend = (size_t)(e - buf) + 4;
                p = strchr(buf, ' ');
                if (!p) { set_err(err, es, "bad server reply"); goto out; }
                *status = (int)ls_atoll(p + 1);
                for (p = strstr(buf, "\r\n"); p && p < buf + hend - 2; p = strstr(p + 2, "\r\n")) {
                    if (!ls_strncasecmp(p + 2, "content-length:", 15)) clen = ls_atoll(p + 17);
                    else if (!ls_strncasecmp(p + 2, "transfer-encoding:", 18) && ls_strcasestr(p + 20, "chunked")) chunked = 1;
                    else if (!ls_strncasecmp(p + 2, "location:", 9)) {
                        const char *v = p + 11;
                        size_t k = 0;
                        while (*v == ' ') v++;
                        while (*v && *v != '\r' && k < ls - 1) loc[k++] = *v++;
                        loc[k] = 0;
                    }
                }
                if (clen > max_bytes) { set_err(err, es, "download too large"); goto out; }
            }
        }
        if (hend && prog) prog(ud, (long long)(have - hend), clen);
        if (hend && clen >= 0 && (long long)(have - hend) >= clen) break;
    }
    if (!hend) { set_err(err, es, "empty reply from server"); goto out; }
    {
        size_t bl = have - hend;
        char *rb = (char *)malloc(bl + 1);
        memcpy(rb, buf + hend, bl);
        rb[bl] = 0;
        if (chunked) dechunk(rb, &bl);
        *body = rb;
        *len = (long)bl;
    }
    rc = (*status >= 300 && *status < 400 && loc[0]) ? 1 : 0;
out:
    free(buf);
    ls_tls_close(t);
    close(fd);
    return rc;
}

int ls_https_get(const char *url, const char *ca_file, long max_bytes, ls_dl_progress prog, void *ud,
                 char **body, long *len, int *status, char *err, size_t es)
{
    char cur[1100], loc[1100];
    int hops;

    signal(SIGPIPE, SIG_IGN);
    *body = NULL;
    *len = 0;
    *status = 0;
    err[0] = 0;
    strncpy(cur, url, sizeof cur - 1);
    cur[sizeof cur - 1] = 0;
    for (hops = 0; hops < 6; hops++) {
        int r;
        loc[0] = 0;
        r = fetch_once(cur, ca_file, max_bytes, prog, ud, body, len, status, loc, sizeof loc, err, es);
        if (r <= 0) return r;
        /* redirect: absolute https URL, or a path on the same host */
        free(*body);
        *body = NULL;
        if (loc[0] == '/') {
            const char *slash = strchr(cur + 8, '/');
            size_t hl = slash ? (size_t)(slash - cur) : strlen(cur);
            char tmp[1100];
            if (hl + strlen(loc) >= sizeof tmp) { set_err(err, es, "redirect too long"); return -1; }
            memcpy(tmp, cur, hl);
            strcpy(tmp + hl, loc);
            strcpy(cur, tmp);
        } else {
            strcpy(cur, loc);
        }
    }
    set_err(err, es, "too many redirects");
    return -1;
}
