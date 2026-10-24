/* Carbon backend for the LocalSend core: Mac OS 9.1+ with CarbonLib, and Mac OS X on PowerPC.
 * Open Transport in synchronous non-blocking mode, polled in loops that yield to other cooperative threads
 * (the same approach as Quackify Classic's sockets), File Manager with FSSpecs, Thread Manager for the core's threads.
 * The application's event loop must call YieldToAnyThread() on every pass. */
#include <Carbon.h>

#include <stdlib.h>
#include <string.h>

#include "ls_plat.h"
#include "ot_decl.h"
#include "plat_carbon.h"

/* Exported by CarbonLib but missing from the cross-compiler's headers; declared as in Apple's. */
typedef struct UnsignedWide { UInt32 hi; UInt32 lo; } UnsignedWide;
void Microseconds(UnsignedWide *microseconds);
OSErr GetEOF(short refNum, long *logEOF);

#define READ_TIMEOUT_S 600.0
#define THREAD_STACK 131072L
#define FOLDER_DESKTOP 0x6465736B   /* 'desk' */

struct plat_conn { EndpointRef ep; double grace_until; };
struct plat_listener { EndpointRef ep; };
struct plat_udp { EndpointRef ep; };
struct plat_file { short ref; FSSpec spec; };

static OTClientContextPtr gCtx = NULL;
static int gInited = 0;

/* ------------------------------------------------------------------ helpers */

static void ip_to_str(UInt32 h, char *out)
{
    int i, n = 0;
    for (i = 3; i >= 0; i--) {
        unsigned v = (unsigned)((h >> (i * 8)) & 255);
        char t[4];
        int k = 0;
        if (v >= 100) t[k++] = (char)('0' + v / 100);
        if (v >= 10) t[k++] = (char)('0' + (v / 10) % 10);
        t[k++] = (char)('0' + v % 10);
        memcpy(out + n, t, (size_t)k);
        n += k;
        if (i) out[n++] = '.';
    }
    out[n] = 0;
}

static int str_to_ip(const char *s, UInt32 *host)
{
    unsigned long v = 0;
    int parts = 0;
    while (*s) {
        unsigned long p = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            p = p * 10 + (unsigned long)(*s - '0');
            s++;
            digits++;
        }
        if (!digits || p > 255) return -1;
        v = (v << 8) | p;
        parts++;
        if (*s == '.') s++;
        else if (*s) return -1;
    }
    if (parts != 4) return -1;
    *host = (UInt32)v;
    return 0;
}

static EndpointRef open_ep(const char *cfg, ls_OSStatus *err)
{
    EndpointRef ep = OTOpenEndpointInContext(OTCreateConfiguration(cfg), 0, NULL, err, gCtx);
    if (*err == kOTNoError && ep) {
        OTSetSynchronous(ep);
        OTSetNonBlocking(ep);
    }
    return ep;
}

static OTResult bind_ep(EndpointRef ep, int port, int qlen)
{
    InetAddress addr, ret;
    TBind req, rsp;
    memset(&addr, 0, sizeof addr);
    memset(&ret, 0, sizeof ret);
    addr.fAddressType = kOTAF_INET;
    addr.fPort = (UInt16)port;
    addr.fHost = 0;
    req.addr.buf = (UInt8 *)&addr;
    req.addr.len = sizeof addr;
    req.addr.maxlen = sizeof addr;
    req.qlen = (UInt32)qlen;
    rsp.addr.buf = (UInt8 *)&ret;
    rsp.addr.len = 0;
    rsp.addr.maxlen = sizeof ret;
    rsp.qlen = 0;
    return OTBind(ep, &req, &rsp);
}

void plat_init(void)
{
    if (gInited) return;
    if (InitOpenTransportInContext(kInitOTForApplicationMask, &gCtx) == kOTNoError) gInited = 1;
}

double plat_now(void)
{
    return (double)TickCount() / 60.0;
}

unsigned long plat_rand32(void)
{
    static unsigned long s = 0;
    UnsignedWide us;
    Microseconds(&us);
    if (s == 0) {
        s = us.lo ^ (us.hi << 7) ^ (unsigned long)TickCount() ^ 0x9E3779B9UL;
        if (s == 0) s = 12345;
    }
    s ^= us.lo;            /* mix in the clock on every call */
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

/* ----------------------------------------------------- threads and locks */

void *plat_mutex_new(void) { return malloc(1); }
void plat_mutex_lock(void *m) { (void)m; }       /* cooperative threads: no preemption */
void plat_mutex_unlock(void *m) { (void)m; }

typedef struct {
    plat_thread_fn fn;
    void *arg;
    volatile int *done;      /* NULL for detached threads */
} tstart;

static void *thread_entry(void *param)
{
    tstart t = *(tstart *)param;
    free(param);
    t.fn(t.arg);
    if (t.done) *t.done = 1;
    return NULL;
}

typedef struct { volatile int done; } thandle;

int plat_thread_start(plat_thread_fn fn, void *arg, void **handle)
{
    ThreadID id;
    OSErr err;
    tstart *t = (tstart *)malloc(sizeof *t);
    thandle *h = NULL;
    t->fn = fn;
    t->arg = arg;
    t->done = NULL;
    if (handle) {
        h = (thandle *)malloc(sizeof *h);
        h->done = 0;
        t->done = &h->done;
    }
    err = NewThread(kCooperativeThread, thread_entry, t, THREAD_STACK, kCreateIfNeeded, NULL, &id);
    if (err != noErr) {
        free(t);
        free(h);
        return -1;
    }
    if (handle) *handle = h;
    return 0;
}

void plat_thread_join(void *handle)
{
    thandle *h = (thandle *)handle;
    if (!h) return;
    while (!h->done) YieldToAnyThread();
    free(h);
}

/* -------------------------------------------------------------------- UDP */

plat_udp *plat_udp_open(int port, const char *group)
{
    ls_OSStatus err;
    plat_udp *u;
    EndpointRef ep = open_ep("udp", &err);
    UInt32 g;
    UInt8 optbuf[sizeof(TOptionHeader) + sizeof(TIPAddMulticast)];
    UInt8 rspbuf[sizeof(TOptionHeader) + sizeof(TIPAddMulticast)];
    TOptionHeader *hdr = (TOptionHeader *)optbuf;
    TIPAddMulticast *mc = (TIPAddMulticast *)(optbuf + sizeof(TOptionHeader));
    TOptMgmt req, rsp;

    if (err != kOTNoError || !ep) return NULL;
    if (bind_ep(ep, port, 0) != kOTNoError) {
        OTCloseProvider(ep);
        return NULL;
    }
    if (str_to_ip(group, &g) == 0) {
        hdr->len = sizeof(TOptionHeader) + sizeof(TIPAddMulticast);
        hdr->level = INET_IP;
        hdr->name = kIP_ADD_MEMBERSHIP;
        hdr->status = 0;
        mc->multicastGroupAddress = g;
        mc->interfaceAddress = 0;
        req.opt.buf = optbuf;
        req.opt.len = hdr->len;
        req.opt.maxlen = sizeof optbuf;
        req.flags = T_NEGOTIATE;
        rsp.opt.buf = rspbuf;
        rsp.opt.len = 0;
        rsp.opt.maxlen = sizeof rspbuf;
        rsp.flags = 0;
        OTOptionManagement(ep, &req, &rsp);
    }
    u = (plat_udp *)malloc(sizeof *u);
    u->ep = ep;
    return u;
}

int plat_udp_send(plat_udp *u, const char *ip, int port, const void *buf, size_t n)
{
    InetAddress dst;
    TUnitData ud;
    UInt32 h;
    OTResult r;
    if (str_to_ip(ip, &h) != 0) return -1;
    memset(&dst, 0, sizeof dst);
    dst.fAddressType = kOTAF_INET;
    dst.fPort = (UInt16)port;
    dst.fHost = h;
    ud.addr.buf = (UInt8 *)&dst;
    ud.addr.len = sizeof dst;
    ud.addr.maxlen = sizeof dst;
    ud.opt.buf = NULL;
    ud.opt.len = 0;
    ud.opt.maxlen = 0;
    ud.udata.buf = (UInt8 *)buf;
    ud.udata.len = (OTByteCount)n;
    ud.udata.maxlen = (OTByteCount)n;
    r = OTSndUData(u->ep, &ud);
    if (r == kOTLookErr && OTLook(u->ep) == T_UDERR) OTRcvUDErr(u->ep, NULL);
    return r == kOTNoError ? 0 : -1;
}

long plat_udp_recv(plat_udp *u, void *buf, size_t n, char *ip_out, int timeout_ms)
{
    double end = plat_now() + timeout_ms / 1000.0;
    InetAddress from;
    TUnitData ud;
    OTFlags flags;
    OTResult r;

    for (;;) {
        memset(&from, 0, sizeof from);
        ud.addr.buf = (UInt8 *)&from;
        ud.addr.len = 0;
        ud.addr.maxlen = sizeof from;
        ud.opt.buf = NULL;
        ud.opt.len = 0;
        ud.opt.maxlen = 0;
        ud.udata.buf = (UInt8 *)buf;
        ud.udata.len = 0;
        ud.udata.maxlen = (OTByteCount)n;
        flags = 0;
        r = OTRcvUData(u->ep, &ud, &flags);
        if (r == kOTNoError) {
            ip_to_str(from.fHost, ip_out);
            return (long)ud.udata.len;
        }
        if (r == kOTLookErr) {
            if (OTLook(u->ep) == T_UDERR) OTRcvUDErr(u->ep, NULL);
            YieldToAnyThread();
            continue;
        }
        if (r != kOTNoDataErr) return -1;
        if (plat_now() >= end) return 0;
        YieldToAnyThread();
    }
}

void plat_udp_close(plat_udp *u)
{
    if (!u) return;
    OTCloseProvider(u->ep);
    free(u);
}

/* -------------------------------------------------------------------- TCP */

plat_listener *plat_tcp_listen(int port)
{
    ls_OSStatus err;
    plat_listener *l;
    EndpointRef ep = open_ep("tcp", &err);
    if (err != kOTNoError || !ep) return NULL;
    /* queue length 1: one pending connection at a time keeps accepting simple */
    if (bind_ep(ep, port, 1) != kOTNoError) {
        OTCloseProvider(ep);
        return NULL;
    }
    l = (plat_listener *)malloc(sizeof *l);
    l->ep = ep;
    return l;
}

plat_conn *plat_tcp_accept(plat_listener *l, char *ip_out, int timeout_ms)
{
    double end = plat_now() + timeout_ms / 1000.0;
    TCall call;
    InetAddress peer;
    OTResult r;
    ls_OSStatus e2;
    EndpointRef nep;
    plat_conn *c;
    double until;

    for (;;) {
        memset(&peer, 0, sizeof peer);
        memset(&call, 0, sizeof call);
        call.addr.buf = (UInt8 *)&peer;
        call.addr.maxlen = sizeof peer;
        r = OTListen(l->ep, &call);
        if (r == kOTNoError) break;
        if (r == kOTNoDataErr) {
            if (plat_now() >= end) return NULL;
            YieldToAnyThread();
            continue;
        }
        if (r == kOTLookErr) {
            if (OTLook(l->ep) == T_DISCONNECT) OTRcvDisconnect(l->ep, NULL);
            YieldToAnyThread();
            continue;
        }
        return NULL;
    }

    nep = open_ep("tcp", &e2);
    if (e2 != kOTNoError || !nep) return NULL;
    if (OTBind(nep, NULL, NULL) != kOTNoError) {
        OTCloseProvider(nep);
        return NULL;
    }
    r = OTAccept(l->ep, nep, &call);
    if (r != kOTNoError) {
        if (r == kOTLookErr && OTLook(l->ep) == T_DISCONNECT) OTRcvDisconnect(l->ep, NULL);
        OTCloseProvider(nep);
        return NULL;
    }
    /* OTAccept has succeeded: use the connection at once. (Waiting for a T_PASSCON event does not work when polling:
     * it never shows up in OTLook, and the first version of this code dropped every connection after 10 seconds.)
     * The first reads and writes tolerate a short "not connected yet" state. */
    until = plat_now() + 0.3;
    while (plat_now() < until) {
        OTResult look = OTLook(nep);
        if (look == T_PASSCON) break;
        if (look == T_DISCONNECT) {
            OTRcvDisconnect(nep, NULL);
            OTCloseProvider(nep);
            return NULL;
        }
        YieldToAnyThread();
    }
    ip_to_str(peer.fHost, ip_out);
    c = (plat_conn *)malloc(sizeof *c);
    c->ep = nep;
    c->grace_until = plat_now() + 5.0;
    return c;
}

void plat_listener_close(plat_listener *l)
{
    if (!l) return;
    OTCloseProvider(l->ep);
    free(l);
}

plat_conn *plat_tcp_connect(const char *ip, int port, int timeout_s)
{
    InetAddress dst;
    TCall call;
    UInt32 h;
    ls_OSStatus err;
    EndpointRef ep;
    OTResult r;
    double started;
    plat_conn *c;

    if (str_to_ip(ip, &h) != 0) return NULL;
    ep = open_ep("tcp", &err);
    if (err != kOTNoError || !ep) return NULL;
    if (OTBind(ep, NULL, NULL) != kOTNoError) {
        OTCloseProvider(ep);
        return NULL;
    }
    memset(&dst, 0, sizeof dst);
    memset(&call, 0, sizeof call);
    dst.fAddressType = kOTAF_INET;
    dst.fPort = (UInt16)port;
    dst.fHost = h;
    call.addr.len = sizeof dst;
    call.addr.maxlen = sizeof dst;
    call.addr.buf = (UInt8 *)&dst;

    started = plat_now();
    r = OTConnect(ep, &call, NULL);
    while (r == kOTNoDataErr || r == kOTLookErr || r == kOTOutStateErr) {
        if (r == kOTLookErr) {
            OTResult look = OTLook(ep);
            if (look == T_DISCONNECT) {
                OTRcvDisconnect(ep, NULL);
                OTCloseProvider(ep);
                return NULL;
            }
            if (look == T_CONNECT) {
                r = OTRcvConnect(ep, NULL);
                if (r == kOTNoError) break;
            }
        } else {
            r = OTRcvConnect(ep, NULL);
            if (r == kOTNoError) break;
        }
        if (plat_now() - started > (double)timeout_s + 10.0) {
            OTCloseProvider(ep);
            return NULL;
        }
        YieldToAnyThread();
        if (r != kOTNoDataErr && r != kOTLookErr && r != kOTOutStateErr) break;
    }
    if (r != kOTNoError) {
        OTCloseProvider(ep);
        return NULL;
    }
    c = (plat_conn *)malloc(sizeof *c);
    c->ep = ep;
    c->grace_until = 0;
    return c;
}

static long conn_read_poll(plat_conn *c, void *buf, size_t n, double deadline)
{
    OTFlags flags;
    OTResult r;
    for (;;) {
        flags = 0;
        r = OTRcv(c->ep, buf, (OTByteCount)n, &flags);
        if (r > 0) return (long)r;
        if (r == kOTLookErr) {
            OTResult look = OTLook(c->ep);
            if (look == T_DISCONNECT) {
                OTRcvDisconnect(c->ep, NULL);
                return 0;
            }
            if (look == T_ORDREL) {
                OTRcvOrderlyDisconnect(c->ep);
                return 0;
            }
            /* T_PASSCON, T_GODATA and the like need no action: try again */
        } else if (r == kOTOutStateErr && plat_now() < c->grace_until) {
            /* just accepted: not in the data-transfer state yet */
        } else if (r != kOTNoDataErr && r != 0) {
            return -1;
        }
        if (plat_now() >= deadline) return -1;
        YieldToAnyThread();
    }
}

long plat_conn_read(plat_conn *c, void *buf, size_t n)
{
    return conn_read_poll(c, buf, n, plat_now() + READ_TIMEOUT_S);
}

long plat_conn_write(plat_conn *c, const void *buf, size_t n)
{
    OTResult r;
    double started = plat_now();
    for (;;) {
        r = OTSnd(c->ep, (void *)buf, (OTByteCount)n, 0);
        if (r > 0) return (long)r;
        if (r == kOTFlowErr || r == kOTNoDataErr || r == 0 || (r == kOTOutStateErr && plat_now() < c->grace_until)) {
            if (plat_now() - started > 60.0) return -1;
            YieldToAnyThread();
            continue;
        }
        if (r == kOTLookErr) {
            OTResult look = OTLook(c->ep);
            if (look == T_DISCONNECT) OTRcvDisconnect(c->ep, NULL);
            else if (look == T_ORDREL) OTRcvOrderlyDisconnect(c->ep);
            else if (look == T_GODATA) { YieldToAnyThread(); continue; }   /* flow control lifted */
        }
        return -1;
    }
}

int plat_conn_peek(plat_conn *c)
{
    (void)c;
    return 256;      /* no TLS here: always plain HTTP */
}

int plat_conn_fd(plat_conn *c)
{
    (void)c;
    return -1;
}

void plat_conn_close(plat_conn *c)
{
    char scratch[256];
    if (!c) return;
    if (OTSndOrderlyDisconnect(c->ep) == kOTNoError) {
        /* let the peer read everything we sent before the stack tears the connection down */
        double end = plat_now() + 2.0;
        while (conn_read_poll(c, scratch, sizeof scratch, end) > 0) {
        }
    }
    OTCloseProvider(c->ep);
    free(c);
}

/* ------------------------------------------------------------------ files */

static void to_pascal(const char *s, unsigned char *p)
{
    size_t n = strlen(s);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

/* "dir" is either "#<volume>:<folder id>" (from plat_carbon_desktop_dir) or a full HFS path. */
static OSErr spec_in_dir(const char *dir, const char *name, FSSpec *spec)
{
    unsigned char p[256];
    if (dir[0] == '#') {
        long vol = 0, id = 0;
        const char *s = dir + 1;
        int neg = 0;
        if (*s == '-') { neg = 1; s++; }
        while (*s >= '0' && *s <= '9') vol = vol * 10 + (*s++ - '0');
        if (neg) vol = -vol;
        if (*s == ':') s++;
        while (*s >= '0' && *s <= '9') id = id * 10 + (*s++ - '0');
        to_pascal(name, p);
        return FSMakeFSSpec((short)vol, id, p, spec);
    } else {
        char full[300];
        size_t dl = strlen(dir);
        strncpy(full, dir, sizeof full - 1);
        full[sizeof full - 1] = 0;
        if (dl && full[dl - 1] != ':' && dl + 1 < sizeof full) { full[dl] = ':'; full[dl + 1] = 0; }
        if (strlen(full) + strlen(name) + 1 >= sizeof full) return -1;
        strcat(full, name);
        to_pascal(full, p);
        return FSMakeFSSpec(0, 0, p, spec);
    }
}

void plat_mkdir(const char *dir) { (void)dir; }

/* HFS names are limited to 31 characters and may not contain a colon. */
static void hfs_name(const char *in, char *out)
{
    char base[256], ext[40];
    const char *dot = strrchr(in, '.');
    size_t bl, el, i;

    ext[0] = 0;
    if (dot && dot != in && strlen(dot) <= 8) {
        strcpy(ext, dot);
        bl = (size_t)(dot - in);
    } else {
        bl = strlen(in);
    }
    el = strlen(ext);
    if (bl + el > 31) bl = 31 - el;
    memcpy(base, in, bl);
    base[bl] = 0;
    for (i = 0; i < bl; i++)
        if (base[i] == ':') base[i] = '-';
    strcpy(out, base);
    strcat(out, ext);
}

static void type_for(const char *name, OSType *type, OSType *creator)
{
    size_t n = strlen(name);
    *type = 0x3F3F3F3FUL;        /* '????' written numerically: the literal contains a trigraph */
    *creator = 0x3F3F3F3FUL;
    if (n > 4 && (name[n - 4] == '.') && (name[n - 3] == 't' || name[n - 3] == 'T')
        && (name[n - 2] == 'x' || name[n - 2] == 'X') && (name[n - 1] == 't' || name[n - 1] == 'T')) {
        *type = 0x54455854UL;    /* 'TEXT' */
        *creator = 0x74747874UL; /* 'ttxt' */
    }
}

plat_file *plat_file_create_unique(const char *dir, const char *name, char *path_out, size_t ps)
{
    char first[256];
    FSSpec spec;
    OSErr err;
    short ref;
    OSType type, creator;
    int n;
    plat_file *f;

    hfs_name(name, first);
    type_for(first, &type, &creator);
    for (n = 0; n < 200; n++) {
        char nm[64];
        if (n == 0) {
            strcpy(nm, first);
        } else {
            /* "name 2.ext": rebuild with a suffix, keeping within 31 characters */
            char base[64], ext[40], num[16];
            const char *dot = strrchr(first, '.');
            size_t bl, el;
            int k = 0, v = n + 1;
            if (dot && dot != first) {
                strcpy(ext, dot);
                bl = (size_t)(dot - first);
            } else {
                ext[0] = 0;
                bl = strlen(first);
            }
            el = strlen(ext);
            num[k++] = ' ';
            if (v >= 100) num[k++] = (char)('0' + v / 100);
            if (v >= 10) num[k++] = (char)('0' + (v / 10) % 10);
            num[k++] = (char)('0' + v % 10);
            num[k] = 0;
            if (bl + el + (size_t)k > 31) bl = 31 - el - (size_t)k;
            memcpy(base, first, bl);
            base[bl] = 0;
            strcpy(nm, base);
            strcat(nm, num);
            strcat(nm, ext);
        }
        err = spec_in_dir(dir, nm, &spec);
        if (err == noErr) continue;            /* already exists */
        if (err != fnfErr) return NULL;
        err = FSpCreate(&spec, creator, type, smSystemScript);
        if (err != noErr) return NULL;
        err = FSpOpenDF(&spec, fsWrPerm, &ref);
        if (err != noErr) {
            FSpDelete(&spec);
            return NULL;
        }
        strncpy(path_out, nm, ps - 1);         /* the file's name; the app shows it as "Received <name>" */
        path_out[ps - 1] = 0;
        f = (plat_file *)malloc(sizeof *f);
        f->ref = ref;
        f->spec = spec;
        return f;
    }
    return NULL;
}

long plat_file_write(plat_file *f, const void *buf, size_t n)
{
    long count = (long)n;
    OSErr err = FSWrite(f->ref, &count, buf);
    return err == noErr ? count : -1;
}

void plat_file_close(plat_file *f)
{
    if (!f) return;
    FSClose(f->ref);
    free(f);
}

void plat_file_remove(const char *path)
{
    (void)path;      /* a bare name cannot be resolved here; plat_file_abort is used for failed transfers */
}

void plat_file_abort(plat_file *f, const char *path)
{
    (void)path;
    if (!f) return;
    FSClose(f->ref);
    FSpDelete(&f->spec);
    free(f);
}

plat_file *plat_file_open_read(const char *path)
{
    FSSpec spec;
    short ref;
    plat_file *f;
    unsigned char p[256];
    to_pascal(path, p);
    if (FSMakeFSSpec(0, 0, p, &spec) != noErr) return NULL;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return NULL;
    f = (plat_file *)malloc(sizeof *f);
    f->ref = ref;
    return f;
}

long plat_file_read(plat_file *f, void *buf, size_t n)
{
    long count = (long)n;
    OSErr err = FSRead(f->ref, &count, buf);
    if (err == noErr || err == eofErr) return count;     /* count is 0 at end of file */
    return -1;
}

int plat_file_size(const char *path, long long *size)
{
    FSSpec spec;
    short ref;
    long eof;
    unsigned char p[256];
    to_pascal(path, p);
    if (FSMakeFSSpec(0, 0, p, &spec) != noErr) return -1;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return -1;
    if (GetEOF(ref, &eof) != noErr) {
        FSClose(ref);
        return -1;
    }
    FSClose(ref);
    *size = (long long)eof;
    return 0;
}

const char *plat_basename(const char *path)
{
    const char *b = strrchr(path, ':');
    return b ? b + 1 : path;
}

/* ---------------------------------------------------- app-level helpers */

int plat_carbon_desktop_dir(char *out, int size)
{
    short vol;
    long dir;
    char tmp[48];
    int n = 0;
    long v;
    if (FindFolder(-32768 /* kOnSystemDisk */, FOLDER_DESKTOP, 1 /* kCreateFolder */, &vol, &dir) != noErr) return -1;
    tmp[n++] = '#';
    v = vol;
    if (v < 0) { tmp[n++] = '-'; v = -v; }
    {
        char d[16];
        int k = 0;
        if (v == 0) d[k++] = '0';
        while (v > 0) { d[k++] = (char)('0' + v % 10); v /= 10; }
        while (k) tmp[n++] = d[--k];
    }
    tmp[n++] = ':';
    {
        char d[16];
        int k = 0;
        long id = dir;
        if (id == 0) d[k++] = '0';
        while (id > 0) { d[k++] = (char)('0' + id % 10); id /= 10; }
        while (k) tmp[n++] = d[--k];
    }
    tmp[n] = 0;
    if (n + 1 > size) return -1;
    memcpy(out, tmp, (size_t)n + 1);
    return 0;
}

int plat_carbon_local_ip(char *out, int size)
{
    UInt32 buffer[256];
    ls_OSStatus err;
    OTClientContextPtr ctx = NULL;
    int ok = 0;
    if (size < 16) return -1;
    out[0] = 0;
    err = InitOpenTransportInContext(kInitOTForApplicationMask, &ctx);
    if (err != kOTNoError) return -1;
    memset(buffer, 0, sizeof buffer);
    if (OTInetGetInterfaceInfo(buffer, -1) == kOTNoError && buffer[0] != 0) {
        ip_to_str(buffer[0], out);
        ok = 1;
    }
    CloseOpenTransportInContext(ctx);
    return ok ? 0 : -1;
}
