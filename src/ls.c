/* LocalSend v2 protocol core. Plain C89, all system access goes through
 * ls_plat.h so the same code runs on Mac OS X and Mac OS 9. */
#include "ls.h"
#include "ls_json.h"
#include "ls_tls.h"
#include "ls_plat.h"
#include "ls_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define MCAST_ADDR "224.0.0.167"
#define MAX_SESSIONS 8
#define MAX_JSON_BODY (8 * 1024 * 1024)
#define API "/api/localsend/v2"
#define IO_CHUNK 32768

typedef struct {
    char id[64];
    char name[256];
    long long size;
    char token[40];
} sfile;

typedef struct {
    int used;
    char id[40];
    sfile *files;
    int n;
} session;

struct ls_ctx {
    char alias[64];
    char fingerprint[72];
    char dir[512];
    int port;
    int use_tls;
    ls_callbacks cb;
    plat_udp *udp;
    plat_listener *lst;
    volatile int running;
    void *tu, *tt;
    void *mu;
    ls_peer peers[LS_MAX_PEERS];
    int npeers;
    session sess[MAX_SESSIONS];
    int sess_next;
};

/* ------------------------------------------------------------------ utils */

static void ls_log(ls_ctx *c, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    if (!c->cb.log) return;
    va_start(ap, fmt);
#ifdef LS_NO_VSNPRINTF
    {
        char t[1024];
        vsprintf(t, fmt, ap);
        strncpy(b, t, sizeof b - 1);
        b[sizeof b - 1] = 0;
    }
#else
    vsnprintf(b, sizeof b, fmt, ap);
#endif
    va_end(ap);
    c->cb.log(c->cb.ud, b);
}

static void rand_hex(char *out, int nbytes)
{
    static const char h[] = "0123456789abcdef";
    int i;
    for (i = 0; i < nbytes; i++) {
        unsigned r = (unsigned)(plat_rand32() >> 3) & 255;
        out[2 * i] = h[r >> 4];
        out[2 * i + 1] = h[r & 15];
    }
    out[2 * nbytes] = 0;
}

static void copy_str(char *dst, size_t n, const char *src)
{
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = 0;
}

typedef struct { char *p; size_t n, cap; } sbuf;

static void sb_add(sbuf *b, const char *s, size_t l)
{
    if (b->n + l + 1 > b->cap) {
        b->cap = (b->n + l + 1) * 2;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, l);
    b->n += l;
    b->p[b->n] = 0;
}

static void sb_str(sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_int(sbuf *b, long long v)
{
    char t[24];
    ls_lltoa(v, t);
    sb_str(b, t);
}

static void sb_jstr(sbuf *b, const char *s)
{
    sb_add(b, "\"", 1);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            char t[2];
            t[0] = '\\';
            t[1] = (char)ch;
            sb_add(b, t, 2);
        } else if (ch < 0x20) {
            char t[8];
            ls_snprintf(t, sizeof t, "\\u%04x", (unsigned)ch);
            sb_str(b, t);
        } else {
            sb_add(b, (const char *)&ch, 1);
        }
    }
    sb_add(b, "\"", 1);
}

static void device_json(ls_ctx *c, sbuf *b, int announce)
{
    sb_str(b, "{\"alias\":");
    sb_jstr(b, c->alias);
    sb_str(b, ",\"version\":\"2.0\",\"deviceModel\":\"PowerPC Mac\",\"deviceType\":\"desktop\",\"fingerprint\":");
    sb_jstr(b, c->fingerprint);
    sb_str(b, ",\"port\":");
    sb_int(b, c->port);
    sb_str(b, ",\"protocol\":\"");
    sb_str(b, c->use_tls ? "https" : "http");
    sb_str(b, "\",\"download\":false");
    if (announce == 1) sb_str(b, ",\"announce\":true");
    else if (announce == 2) sb_str(b, ",\"announce\":false");
    sb_str(b, "}");
}

/* ------------------------------------------------------------------ peers */

static int peer_from_json(const jval *j, const char *ip, ls_peer *p)
{
    const char *fp = json_str(j, "fingerprint", NULL);
    if (!j || j->t != J_OBJ || !fp) return 0;
    memset(p, 0, sizeof *p);
    copy_str(p->alias, sizeof p->alias, json_str(j, "alias", "?"));
    copy_str(p->ip, sizeof p->ip, ip);
    copy_str(p->fingerprint, sizeof p->fingerprint, fp);
    copy_str(p->protocol, sizeof p->protocol, json_str(j, "protocol", "https"));
    copy_str(p->deviceType, sizeof p->deviceType, json_str(j, "deviceType", "desktop"));
    copy_str(p->deviceModel, sizeof p->deviceModel, json_str(j, "deviceModel", ""));
    p->port = (int)json_num(j, "port", LS_PORT);
    return 1;
}

static void add_peer(ls_ctx *c, const ls_peer *p)
{
    int i, changed = 1;
    if (!strcmp(p->fingerprint, c->fingerprint)) return;
    plat_mutex_lock(c->mu);
    for (i = 0; i < c->npeers; i++)
        if (!strcmp(c->peers[i].fingerprint, p->fingerprint)) break;
    if (i < c->npeers) {
        changed = memcmp(&c->peers[i], p, sizeof *p) != 0;
        c->peers[i] = *p;
    } else if (c->npeers < LS_MAX_PEERS) {
        c->peers[c->npeers++] = *p;
    } else {
        plat_mutex_unlock(c->mu);
        return;
    }
    plat_mutex_unlock(c->mu);
    if (changed && c->cb.peer) c->cb.peer(c->cb.ud, p);
}

int ls_peer_list(ls_ctx *c, ls_peer *out, int max)
{
    int n;
    plat_mutex_lock(c->mu);
    n = c->npeers < max ? c->npeers : max;
    memcpy(out, c->peers, n * sizeof(ls_peer));
    plat_mutex_unlock(c->mu);
    return n;
}

/* ---------------------------------------------------------------- streams */

typedef struct { plat_conn *pc; ls_tls *tls; } stream;

static long st_read(stream *s, void *b, size_t n)
{
    return s->tls ? (long)ls_tls_read(s->tls, b, n) : plat_conn_read(s->pc, b, n);
}

static int send_all(stream *s, const char *b, size_t n)
{
    while (n) {
        long w = s->tls ? (long)ls_tls_write(s->tls, b, n) : plat_conn_write(s->pc, b, n);
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ------------------------------------------------------------ HTTP client */

typedef struct {
    ls_ctx *c;
    const char *name;
    long long total, done;
    double last;
    int sending;
} prog;

static void prog_tick(prog *p, long long add, int force)
{
    double t;
    p->done += add;
    if (!p->c->cb.progress) return;
    t = plat_now();
    if (force || t - p->last > 0.1) {
        p->last = t;
        p->c->cb.progress(p->c->cb.ud, p->name, p->done, p->total, p->sending);
    }
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

/* Performs one HTTP/1.1 request. Body is either `body` or the file at
 * `filepath`. On success *status is set and *resp holds the (malloc'd,
 * NUL-terminated) response body. Returns 0 on success. */
static int http_request(ls_ctx *c, const char *ip, int port, const char *method, const char *path,
                        const char *body, size_t blen, const char *filepath, long long fsize,
                        prog *pg, int *status, char **resp, int use_tls)
{
    plat_conn *pc;
    stream sm;
    sbuf h;
    char *buf = NULL;
    size_t have = 0, cap = 0, hend = 0;
    long long clen = -1;
    int chunked = 0, rc = -1;

    *status = 0;
    *resp = NULL;
    pc = plat_tcp_connect(ip, port, 5);
    if (!pc) return -1;
    sm.pc = pc;
    sm.tls = NULL;
    if (use_tls && !(sm.tls = ls_tls_connect(plat_conn_fd(pc)))) {
        plat_conn_close(pc);
        return -1;
    }

    h.p = NULL;
    h.n = h.cap = 0;
    sb_str(&h, method);
    sb_str(&h, " ");
    sb_str(&h, path);
    sb_str(&h, " HTTP/1.1\r\nHost: ");
    sb_str(&h, ip);
    sb_str(&h, ":");
    sb_int(&h, port);
    sb_str(&h, "\r\nUser-Agent: ppc-localsend\r\nContent-Type: ");
    sb_str(&h, filepath ? "application/octet-stream" : "application/json");
    sb_str(&h, "\r\nContent-Length: ");
    sb_int(&h, filepath ? fsize : (long long)blen);
    sb_str(&h, "\r\nConnection: close\r\n\r\n");
    if (send_all(&sm, h.p, h.n) < 0) { free(h.p); goto out; }
    free(h.p);

    if (filepath) {
        plat_file *f = plat_file_open_read(filepath);
        char *chunk;
        long r;
        if (!f) goto out;
        chunk = (char *)malloc(IO_CHUNK);
        while ((r = plat_file_read(f, chunk, IO_CHUNK)) > 0) {
            if (send_all(&sm, chunk, (size_t)r) < 0) { r = -1; break; }
            if (pg) prog_tick(pg, r, 0);
        }
        free(chunk);
        plat_file_close(f);
        if (r < 0) goto out;
        if (pg) prog_tick(pg, 0, 1);
    } else if (blen && send_all(&sm, body, blen) < 0) {
        goto out;
    }

    for (;;) {
        long r;
        if (have + 4096 + 1 > cap) {
            if (cap > 4 * 1024 * 1024) goto out;
            cap = cap ? cap * 2 : 8192;
            buf = (char *)realloc(buf, cap);
        }
        r = st_read(&sm, buf + have, cap - have - 1);
        if (r < 0) goto out;
        if (r == 0) break;
        have += (size_t)r;
        buf[have] = 0;
        if (!hend) {
            char *e = strstr(buf, "\r\n\r\n");
            if (e) {
                char *p;
                hend = (size_t)(e - buf) + 4;
                /* status line: "HTTP/1.1 200 OK" */
                p = strchr(buf, ' ');
                if (!p) goto out;
                *status = (int)ls_atoll(p + 1);
                for (p = strstr(buf, "\r\n"); p && p < buf + hend - 2; p = strstr(p + 2, "\r\n")) {
                    if (!ls_strncasecmp(p + 2, "content-length:", 15)) clen = ls_atoll(p + 17);
                    if (!ls_strncasecmp(p + 2, "transfer-encoding:", 18) && ls_strcasestr(p + 20, "chunked")) chunked = 1;
                }
                if (*status == 204 || *status == 304) break;
            }
        }
        if (hend && clen >= 0 && (long long)(have - hend) >= clen) break;
    }
    if (!hend) goto out;
    {
        size_t bl = have - hend;
        char *rb = (char *)malloc(bl + 1);
        memcpy(rb, buf + hend, bl);
        rb[bl] = 0;
        if (chunked) dechunk(rb, &bl);
        *resp = rb;
    }
    rc = 0;
out:
    free(buf);
    ls_tls_close(sm.tls);
    plat_conn_close(pc);
    return rc;
}

/* ------------------------------------------------------------ HTTP server */

static void respond(stream *io, int code, const char *body)
{
    const char *reason = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request"
                       : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 409 ? "Conflict" : "Error";
    sbuf h;
    size_t bl = body ? strlen(body) : 0;
    h.p = NULL;
    h.n = h.cap = 0;
    sb_str(&h, "HTTP/1.1 ");
    sb_int(&h, code);
    sb_str(&h, " ");
    sb_str(&h, reason);
    sb_str(&h, "\r\nContent-Type: application/json\r\nContent-Length: ");
    sb_int(&h, (long long)bl);
    sb_str(&h, "\r\nConnection: close\r\n\r\n");
    send_all(io, h.p, h.n);
    free(h.p);
    if (bl) send_all(io, body, bl);
}

static int find_header(const char *h, const char *name, char *out, size_t os)
{
    size_t nl = strlen(name);
    const char *p = strstr(h, "\r\n");
    while (p) {
        p += 2;
        if (!ls_strncasecmp(p, name, nl) && p[nl] == ':') {
            size_t i = 0;
            p += nl + 1;
            while (*p == ' ') p++;
            while (*p && *p != '\r' && i < os - 1) out[i++] = *p++;
            out[i] = 0;
            return 1;
        }
        p = strstr(p, "\r\n");
    }
    return 0;
}

static int qparam(const char *q, const char *key, char *out, size_t os)
{
    size_t kl = strlen(key);
    while (q && *q) {
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            size_t i = 0;
            q += kl + 1;
            while (*q && *q != '&' && i < os - 1) out[i++] = *q++;
            out[i] = 0;
            return 1;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
    return 0;
}

/* Request body reader: handles Content-Length and chunked transfer encoding. */
typedef struct {
    stream *io;
    const char *lp;        /* bytes already received past the headers */
    size_t ln;
    long long remain;      /* Content-Length mode */
    int chunked;
    long long cr;          /* bytes left in the current chunk */
    int eof;
} breader;

static long br_raw(breader *b, void *buf, size_t n)
{
    if (b->ln) {
        size_t k = b->ln < n ? b->ln : n;
        memcpy(buf, b->lp, k);
        b->lp += k;
        b->ln -= k;
        return (long)k;
    }
    return st_read(b->io, buf, n);
}

static int br_byte(breader *b)
{
    unsigned char ch;
    return br_raw(b, &ch, 1) == 1 ? ch : -1;
}

/* Returns bytes read, 0 at the end of the body, <0 on error. */
static long br_read(breader *b, void *buf, size_t n)
{
    long r;
    if (!b->chunked) {
        if (b->remain <= 0) return 0;
        r = br_raw(b, buf, (size_t)b->remain < n ? (size_t)b->remain : n);
        if (r <= 0) return -1;      /* connection ended before the full length */
        b->remain -= r;
        return r;
    }
    while (!b->eof && b->cr == 0) {
        char line[64];
        int i = 0, ch;
        while ((ch = br_byte(b)) != '\n') {
            if (ch < 0 || i >= 62) return -1;
            if (ch != '\r') line[i++] = (char)ch;
        }
        line[i] = 0;
        b->cr = ls_hextoll(line);
        if (b->cr < 0) return -1;
        if (b->cr == 0) {           /* last chunk: skip trailers up to the blank line */
            for (;;) {
                int len = 0;
                while ((ch = br_byte(b)) != '\n' && ch >= 0)
                    if (ch != '\r') len++;
                if (ch < 0 || len == 0) break;
            }
            b->eof = 1;
        }
    }
    if (b->eof) return 0;
    r = br_raw(b, buf, (size_t)b->cr < n ? (size_t)b->cr : n);
    if (r <= 0) return -1;
    b->cr -= r;
    if (b->cr == 0 && (br_byte(b) < 0 || br_byte(b) < 0)) return -1;   /* CRLF after chunk */
    return r;
}

/* Reads the full request body (JSON routes). Returns malloc'd string or NULL. */
static char *read_body(breader *b)
{
    size_t cap = 4096, got = 0;
    char *buf = (char *)malloc(cap);
    for (;;) {
        long r;
        if (got + 2049 > cap) {
            if (cap >= MAX_JSON_BODY) { free(buf); return NULL; }
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        r = br_read(b, buf + got, cap - got - 1);
        if (r < 0) { free(buf); return NULL; }
        if (r == 0) break;
        got += (size_t)r;
    }
    buf[got] = 0;
    return buf;
}

static void safe_name(const char *in, char *out, size_t os)
{
    const char *b = in, *p;
    size_t i = 0;
    for (p = in; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    if (!*b) b = "file";
    if (*b == '.') out[i++] = '_';
    while (*b && i < os - 1) out[i++] = *b++;
    out[i] = 0;
}

static void do_prepare(ls_ctx *c, stream *io, const char *body)
{
    jval *root = json_parse(body), *files, *info;
    ls_file *lf;
    session *s;
    sbuf b;
    const char *from;
    int i, n, accept;

    b.p = NULL;
    b.n = b.cap = 0;
    files = json_get(root, "files");
    info = json_get(root, "info");
    if (!root || !files || files->t != J_OBJ || files->n == 0) {
        respond(io, 400, "{\"message\":\"Invalid body\"}");
        json_free(root);
        return;
    }
    n = files->n;
    from = json_str(info, "alias", "Unknown");
    lf = (ls_file *)calloc((size_t)n, sizeof(ls_file));
    for (i = 0; i < n; i++) {
        jval *f = files->items[i];
        copy_str(lf[i].id, sizeof lf[i].id, json_str(f, "id", files->keys[i]));
        copy_str(lf[i].name, sizeof lf[i].name, json_str(f, "fileName", "file"));
        lf[i].size = (long long)json_num(f, "size", 0);
    }
    accept = c->cb.incoming ? c->cb.incoming(c->cb.ud, from, n, lf) : 1;
    if (!accept) {
        respond(io, 403, "{\"message\":\"Rejected\"}");
        goto done;
    }

    plat_mutex_lock(c->mu);
    s = &c->sess[c->sess_next];
    c->sess_next = (c->sess_next + 1) % MAX_SESSIONS;
    if (s->used) free(s->files);
    memset(s, 0, sizeof *s);
    s->used = 1;
    rand_hex(s->id, 16);
    s->n = n;
    s->files = (sfile *)calloc((size_t)n, sizeof(sfile));
    sb_str(&b, "{\"sessionId\":\"");
    sb_str(&b, s->id);
    sb_str(&b, "\",\"files\":{");
    for (i = 0; i < n; i++) {
        copy_str(s->files[i].id, sizeof s->files[i].id, lf[i].id);
        copy_str(s->files[i].name, sizeof s->files[i].name, lf[i].name);
        s->files[i].size = lf[i].size;
        rand_hex(s->files[i].token, 16);
        if (i) sb_str(&b, ",");
        sb_jstr(&b, lf[i].id);
        sb_str(&b, ":\"");
        sb_str(&b, s->files[i].token);
        sb_str(&b, "\"");
    }
    sb_str(&b, "}}");
    plat_mutex_unlock(c->mu);
    respond(io, 200, b.p);
    free(b.p);
done:
    free(lf);
    json_free(root);
}

static void do_upload(ls_ctx *c, stream *io, const char *query, breader *br)
{
    char sid[64], fid[64], tok[64], name[256], path[1100], safe[256];
    long long size = 0;
    int i, j, ok = 0, bad = 0;
    plat_file *f;
    prog pg;
    char *chunk;
    double t0;
    long long total_bytes;

    name[0] = 0;
    if (!qparam(query, "sessionId", sid, sizeof sid) || !qparam(query, "fileId", fid, sizeof fid)
        || !qparam(query, "token", tok, sizeof tok)) {
        respond(io, 400, "{\"message\":\"Missing parameters\"}");
        return;
    }
    plat_mutex_lock(c->mu);
    for (i = 0; i < MAX_SESSIONS && !ok; i++) {
        session *s = &c->sess[i];
        if (!s->used || strcmp(s->id, sid)) continue;
        for (j = 0; j < s->n; j++)
            if (!strcmp(s->files[j].id, fid) && !strcmp(s->files[j].token, tok)) {
                copy_str(name, sizeof name, s->files[j].name);
                size = s->files[j].size;
                ok = 1;
                break;
            }
    }
    plat_mutex_unlock(c->mu);
    if (!ok) { respond(io, 403, "{\"message\":\"Invalid token\"}"); return; }

    safe_name(name, safe, sizeof safe);
    f = plat_file_create_unique(c->dir, safe, path, sizeof path);
    if (!f) { respond(io, 400, "{\"message\":\"Cannot create file\"}"); return; }

    pg.c = c;
    pg.name = safe;
    pg.total = (!br->chunked && br->remain > 0) ? br->remain : size;
    pg.done = 0;
    pg.last = 0;
    pg.sending = 0;
    t0 = plat_now();
    total_bytes = 0;
    chunk = (char *)malloc(IO_CHUNK);
    for (;;) {
        long r = br_read(br, chunk, IO_CHUNK);
        if (r < 0 || (r > 0 && plat_file_write(f, chunk, (size_t)r) != r)) { bad = 1; break; }
        if (r == 0) break;
        total_bytes += r;
        prog_tick(&pg, r, 0);
    }
    free(chunk);
    if (bad) {
        plat_file_abort(f, path);
        respond(io, 400, "{\"message\":\"Transfer failed\"}");
        ls_log(c, "receive of %s failed", safe);
        return;
    }
    plat_file_close(f);
    prog_tick(&pg, 0, 1);
    respond(io, 200, "{}");
    if (c->cb.received) c->cb.received(c->cb.ud, path);
    {
        double secs = plat_now() - t0;
        long tenths;
        char how[96];
        if (io->tls) ls_tls_info(io->tls, how, sizeof how); else strcpy(how, "plain HTTP (no encryption)");
        if (secs < 0.001) secs = 0.001;
        tenths = (long)(secs * 10.0 + 0.5);
        ls_log(c, "received %s: %ld KB in %ld.%ld s, %ld KB/s, %s", safe, (long)(total_bytes / 1024), tenths / 10,
               tenths % 10, (long)((double)total_bytes / 1024.0 / secs), how);
    }
}

typedef struct { ls_ctx *c; plat_conn *pc; char ip[48]; } conn;

static void handle_conn(ls_ctx *c, plat_conn *pc, const char *ip)
{
    char *hdr = (char *)malloc(16384);
    char method[16], path[1024], tmp[64], *q, *e = NULL, *body, *sp;
    size_t have = 0, hlen;
    long long clen = -1;
    int chunked, first;
    breader br;
    stream sm, *io = &sm;

    sm.pc = pc;
    sm.tls = NULL;
    /* A TLS ClientHello starts with 0x16; anything else is plain HTTP. */
    first = plat_conn_peek(pc);
    if (first < 0) goto out;
    if (first == 0x16 && ls_tls_available()) {
        sm.tls = ls_tls_accept(plat_conn_fd(pc));
        if (!sm.tls) { ls_log(c, "TLS handshake with %s failed", ip); goto out; }
    }
    while (!e) {
        long r;
        if (have >= 16383) { respond(io, 400, NULL); goto out; }
        r = st_read(io, hdr + have, 16383 - have);
        if (r <= 0) goto out;
        have += (size_t)r;
        hdr[have] = 0;
        if (!io->tls && (unsigned char)hdr[0] == 0x16) {   /* a TLS ClientHello, but this build cannot do TLS */
            ls_log(c, "%s tried an encrypted connection. Turn Encryption off in LocalSend's settings.", ip);
            goto out;
        }
        e = strstr(hdr, "\r\n\r\n");
    }
    hlen = (size_t)(e - hdr) + 4;
    e[2] = 0;                       /* header-only view for find_header */

    /* request line: METHOD SP PATH SP HTTP/1.x */
    method[0] = path[0] = 0;
    sp = strchr(hdr, ' ');
    if (!sp || (size_t)(sp - hdr) >= sizeof method) { respond(io, 400, NULL); goto out; }
    memcpy(method, hdr, (size_t)(sp - hdr));
    method[sp - hdr] = 0;
    {
        const char *ps = sp + 1;
        size_t k = 0;
        while (*ps && *ps != ' ' && *ps != '\r' && k < sizeof path - 1) path[k++] = *ps++;
        path[k] = 0;
    }

    if (find_header(hdr, "content-length", tmp, sizeof tmp)) clen = ls_atoll(tmp);
    else clen = 0;
    chunked = find_header(hdr, "transfer-encoding", tmp, sizeof tmp) && ls_strcasestr(tmp, "chunked") != NULL;
    q = strchr(path, '?');
    if (q) *q++ = 0;
    ls_log(c, "%s %s from %s", method, path, ip);     /* path without the query: it holds a secret token */
    body = hdr + hlen;              /* leftover bytes already received */
    memset(&br, 0, sizeof br);
    br.io = io;
    br.lp = body;
    br.ln = have - hlen;
    br.remain = clen;
    br.chunked = chunked;

    if (!strcmp(method, "GET") && !strcmp(path, API "/info")) {
        sbuf b;
        b.p = NULL;
        b.n = b.cap = 0;
        device_json(c, &b, 0);
        respond(io, 200, b.p);
        free(b.p);
    } else if (!strcmp(method, "POST") && !strcmp(path, API "/register")) {
        char *bd = read_body(&br);
        jval *j = json_parse(bd);
        ls_peer p;
        sbuf b;
        b.p = NULL;
        b.n = b.cap = 0;
        if (peer_from_json(j, ip, &p)) add_peer(c, &p);
        device_json(c, &b, 0);
        respond(io, 200, b.p);
        free(b.p);
        json_free(j);
        free(bd);
    } else if (!strcmp(method, "POST") && !strcmp(path, API "/prepare-upload")) {
        char *bd = read_body(&br);
        if (bd) do_prepare(c, io, bd);
        else respond(io, 400, "{\"message\":\"Bad body\"}");
        free(bd);
    } else if (!strcmp(method, "POST") && !strcmp(path, API "/upload")) {
        do_upload(c, io, q, &br);
    } else if (!strcmp(method, "POST") && !strcmp(path, API "/cancel")) {
        char sid[64];
        if (q && qparam(q, "sessionId", sid, sizeof sid)) {
            int i;
            plat_mutex_lock(c->mu);
            for (i = 0; i < MAX_SESSIONS; i++)
                if (c->sess[i].used && !strcmp(c->sess[i].id, sid)) {
                    free(c->sess[i].files);
                    memset(&c->sess[i], 0, sizeof(session));
                }
            plat_mutex_unlock(c->mu);
        }
        respond(io, 200, "{}");
    } else {
        respond(io, 404, "{\"message\":\"Not found\"}");
    }
out:
    ls_tls_close(sm.tls);
    free(hdr);
}

static void conn_thread(void *arg)
{
    conn *cn = (conn *)arg;
    handle_conn(cn->c, cn->pc, cn->ip);
    plat_conn_close(cn->pc);
    free(cn);
}

static void tcp_thread(void *arg)
{
    ls_ctx *c = (ls_ctx *)arg;
    while (c->running) {
        char ip[48];
        plat_conn *pc = plat_tcp_accept(c->lst, ip, 1000);
        conn *cn;
        if (!pc) continue;
        cn = (conn *)calloc(1, sizeof *cn);
        cn->c = c;
        cn->pc = pc;
        copy_str(cn->ip, sizeof cn->ip, ip);
        if (plat_thread_start(conn_thread, cn, NULL) != 0) {
            plat_conn_close(pc);
            free(cn);
        }
    }
}

/* -------------------------------------------------------------- discovery */

static void mcast_send(ls_ctx *c, int announce)
{
    sbuf b;
    b.p = NULL;
    b.n = b.cap = 0;
    device_json(c, &b, announce ? 1 : 2);
    plat_udp_send(c->udp, MCAST_ADDR, LS_PORT, b.p, b.n);
    free(b.p);
}

void ls_announce(ls_ctx *c) { mcast_send(c, 1); }

void ls_set_alias(ls_ctx *c, const char *alias)
{
    if (!alias || !*alias) return;
    plat_mutex_lock(c->mu);
    copy_str(c->alias, sizeof c->alias, alias);
    plat_mutex_unlock(c->mu);
    mcast_send(c, 1);
}

void ls_set_download_dir(ls_ctx *c, const char *dir)
{
    if (!dir || !*dir) return;
    plat_mutex_lock(c->mu);
    copy_str(c->dir, sizeof c->dir, dir);
    plat_mutex_unlock(c->mu);
    plat_mkdir(dir);
}

void ls_set_encryption(ls_ctx *c, int on)
{
    if (on && !ls_tls_available()) return;
    c->use_tls = on ? 1 : 0;
    mcast_send(c, 1);       /* peers learn the new protocol from the announcement */
}

int ls_https_available(void) { return ls_tls_available(); }
const char *ls_https_error(void) { return ls_tls_error(); }

typedef struct { ls_ctx *c; ls_peer p; } regarg;

static void register_thread(void *arg)
{
    regarg *r = (regarg *)arg;
    sbuf b;
    char *resp = NULL;
    int st = 0;
    b.p = NULL;
    b.n = b.cap = 0;
    device_json(r->c, &b, 0);
    if (http_request(r->c, r->p.ip, r->p.port, "POST", API "/register", b.p, b.n, NULL, 0,
                     NULL, &st, &resp, !strcmp(r->p.protocol, "https")) != 0 || st != 200)
        mcast_send(r->c, 0);
    free(resp);
    free(b.p);
    free(r);
}

static void udp_thread(void *arg)
{
    ls_ctx *c = (ls_ctx *)arg;
    char *buf = (char *)malloc(4096);
    double last_ann = plat_now();
    while (c->running) {
        char ip[48];
        long r;
        jval *j;
        ls_peer p;

        if (plat_now() - last_ann > 30) { mcast_send(c, 1); last_ann = plat_now(); }
        r = plat_udp_recv(c->udp, buf, 4095, ip, 1000);
        if (r <= 0) continue;
        buf[r] = 0;
        j = json_parse(buf);
        if (peer_from_json(j, ip, &p) && strcmp(p.fingerprint, c->fingerprint)) {
            jval *an = json_get(j, "announce");
            add_peer(c, &p);
            if (an && an->t == J_BOOL && an->b) {
                regarg *ra = (regarg *)calloc(1, sizeof *ra);
                ra->c = c;
                ra->p = p;
                if (plat_thread_start(register_thread, ra, NULL) != 0) free(ra);
            }
        }
        json_free(j);
    }
    free(buf);
}

/* ------------------------------------------------------------- lifecycle */

ls_ctx *ls_start(const char *alias, const char *download_dir, const char *data_dir, int port,
                 const ls_callbacks *cb)
{
    ls_ctx *c = (ls_ctx *)calloc(1, sizeof *c);

    plat_init();
    copy_str(c->alias, sizeof c->alias, alias);
    copy_str(c->dir, sizeof c->dir, download_dir);
    c->port = port ? port : LS_PORT;
    if (cb) c->cb = *cb;
    /* TLS is always initialised so we can send to HTTPS peers; serving HTTPS
     * (and announcing "https") only happens when a data_dir is given. */
    if (ls_tls_init(data_dir, c->fingerprint) == 0) {
        c->use_tls = data_dir != NULL;
        if (!c->use_tls) rand_hex(c->fingerprint, 32);
    } else {
        rand_hex(c->fingerprint, 32);
        ls_log(c, "HTTPS unavailable: %s", ls_tls_error());
    }

    c->mu = plat_mutex_new();
    plat_mkdir(c->dir);

    c->lst = plat_tcp_listen(c->port);
    if (!c->lst) {
        ls_log(c, "cannot listen on TCP %d", c->port);
        free(c);
        return NULL;
    }
    c->udp = plat_udp_open(LS_PORT, MCAST_ADDR);
    if (!c->udp) {
        ls_log(c, "cannot open UDP %d (multicast discovery)", LS_PORT);
        plat_listener_close(c->lst);
        free(c);
        return NULL;
    }

    c->running = 1;
    plat_thread_start(tcp_thread, c, &c->tt);
    plat_thread_start(udp_thread, c, &c->tu);
    ls_announce(c);
    ls_log(c, "listening on TCP %d (alias \"%s\", %s)", c->port, c->alias, c->use_tls ? "https" : "http");
    return c;
}

void ls_stop(ls_ctx *c)
{
    if (!c) return;
    c->running = 0;
    plat_thread_join(c->tt);
    plat_thread_join(c->tu);
    plat_listener_close(c->lst);
    plat_udp_close(c->udp);
    free(c);
}

/* ---------------------------------------------------------------- sending */

static const char *mime_for(const char *name)
{
    static const char *tab[][2] = {
        { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".png", "image/png" },
        { ".gif", "image/gif" }, { ".txt", "text/plain" }, { ".pdf", "application/pdf" },
        { ".mp4", "video/mp4" }, { ".mp3", "audio/mpeg" }, { ".zip", "application/zip" },
        { ".html", "text/html" }
    };
    const char *dot = strrchr(name, '.');
    size_t i;
    if (dot)
        for (i = 0; i < sizeof tab / sizeof tab[0]; i++)
            if (!ls_strcasecmp(dot, tab[i][0])) return tab[i][1];
    return "application/octet-stream";
}

int ls_send(ls_ctx *c, const ls_peer *p, int n, const char **paths)
{
    sbuf b;
    long long *sizes;
    const char **names;
    char *resp = NULL;
    int i, st = 0, rc = -1, tls;
    jval *root = NULL, *tokens, *sid;
    char sessionId[64], fileId[16], url[512];
    double t_send = 0;

    b.p = NULL;
    b.n = b.cap = 0;
    sessionId[0] = 0;
    tls = !strcmp(p->protocol, "https");
    if (tls && !ls_tls_available()) {
        ls_log(c, "%s needs HTTPS, which is unavailable: %s", p->alias, ls_tls_error());
        return -2;
    }
    sizes = (long long *)calloc((size_t)n, sizeof(long long));
    names = (const char **)calloc((size_t)n, sizeof(char *));

    sb_str(&b, "{\"info\":");
    device_json(c, &b, 0);
    sb_str(&b, ",\"files\":{");
    for (i = 0; i < n; i++) {
        const char *base = plat_basename(paths[i]);
        if (plat_file_size(paths[i], &sizes[i]) != 0) {
            ls_log(c, "not a regular file: %s", paths[i]);
            goto out;
        }
        names[i] = base;
        ls_snprintf(fileId, sizeof fileId, "f%d", i);
        if (i) sb_str(&b, ",");
        sb_jstr(&b, fileId);
        sb_str(&b, ":{\"id\":");
        sb_jstr(&b, fileId);
        sb_str(&b, ",\"fileName\":");
        sb_jstr(&b, base);
        sb_str(&b, ",\"size\":");
        sb_int(&b, sizes[i]);
        sb_str(&b, ",\"fileType\":");
        sb_jstr(&b, mime_for(base));
        sb_str(&b, "}");
    }
    sb_str(&b, "}}");

    ls_log(c, "asking %s (%s) to accept %d file(s)...", p->alias, p->ip, n);
    if (http_request(c, p->ip, p->port, "POST", API "/prepare-upload", b.p, b.n, NULL, 0,
                     NULL, &st, &resp, tls) != 0) {
        ls_log(c, "cannot reach %s:%d", p->ip, p->port);
        goto out;
    }
    if (st == 403) { ls_log(c, "%s declined the transfer", p->alias); rc = -3; goto out; }
    if (st == 204) { ls_log(c, "nothing to transfer"); rc = 0; goto out; }
    if (st != 200) { ls_log(c, "prepare-upload failed (HTTP %d)", st); goto out; }

    root = json_parse(resp);
    sid = json_get(root, "sessionId");
    tokens = json_get(root, "files");
    if (!sid || sid->t != J_STR || !tokens) { ls_log(c, "bad reply from %s", p->alias); goto out; }
    copy_str(sessionId, sizeof sessionId, sid->s);

    for (i = 0; i < n; i++) {
        char *r2 = NULL;
        const char *tok;
        prog pg;
        ls_snprintf(fileId, sizeof fileId, "f%d", i);
        tok = json_str(tokens, fileId, NULL);
        if (!tok) continue;     /* receiver skipped this file */
        ls_snprintf(url, sizeof url, API "/upload?sessionId=%s&fileId=%s&token=%s", sessionId, fileId, tok);
        pg.c = c;
        pg.name = names[i];
        pg.total = sizes[i];
        pg.done = 0;
        pg.last = 0;
        pg.sending = 1;
        t_send = plat_now();
        if (http_request(c, p->ip, p->port, "POST", url, NULL, 0, paths[i], sizes[i], &pg, &st, &r2, tls) != 0
            || st != 200) {
            ls_log(c, "upload of %s failed (HTTP %d)", names[i], st);
            free(r2);
            r2 = NULL;
            ls_snprintf(url, sizeof url, API "/cancel?sessionId=%s", sessionId);
            http_request(c, p->ip, p->port, "POST", url, NULL, 0, NULL, 0, NULL, &st, &r2, tls);
            free(r2);
            goto out;
        }
        free(r2);
        {
            double secs = plat_now() - t_send;
            long tenths;
            if (secs < 0.001) secs = 0.001;
            tenths = (long)(secs * 10.0 + 0.5);
            ls_log(c, "sent %s: %ld KB in %ld.%ld s, %ld KB/s, %s", names[i], (long)(sizes[i] / 1024), tenths / 10,
                   tenths % 10, (long)((double)sizes[i] / 1024.0 / secs), tls ? "HTTPS" : "plain HTTP");
        }
    }
    rc = 0;
out:
    json_free(root);
    free(resp);
    free(b.p);
    free(sizes);
    free(names);
    return rc;
}
