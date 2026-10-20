/* Mac OS 9 backend for the LocalSend core: Open Transport (TCP/UDP), File Manager,
 * Thread Manager (cooperative threads). Build with MPW MrC or CodeWarrior.
 *
 * Every blocking Open Transport call runs in synchronous mode with sync-idle events,
 * and the notifier yields to other threads while it waits, so the UI stays alive.
 * The application's event loop must call YieldToAnyThread() regularly. */
#include "ls_plat.h"
#include "plat_os9.h"

#include <MacTypes.h>
#include <Files.h>
#include <Folders.h>
#include <Threads.h>
#include <Events.h>
#include <Timer.h>
#include <OpenTransport.h>
#include <OpenTransportProviders.h>

#include <stdlib.h>
#include <string.h>

#define READ_TIMEOUT_S 600
#define THREAD_STACK 131072L

struct plat_conn { EndpointRef ep; };
struct plat_listener { EndpointRef ep; };
struct plat_udp { EndpointRef ep; };
struct plat_file { short ref; };

static OTNotifyUPP gNotifier = nil;
static int gInited = 0;

/* --------------------------------------------------------------- helpers */

static pascal void yield_notifier(void *ctx, OTEventCode code, OTResult result, void *cookie)
{
    if (code == kOTSyncIdleEvent) YieldToAnyThread();
}

static void ip_to_str(InetHost host, char *out)
{
    unsigned long h = (unsigned long)host;
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

static int str_to_ip(const char *s, InetHost *host)
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
    *host = (InetHost)v;
    return 0;
}

static EndpointRef open_ep(const char *cfg, OSStatus *err)
{
    EndpointRef ep = OTOpenEndpoint(OTCreateConfiguration(cfg), 0, nil, err);
    if (*err == noErr) {
        OTSetSynchronous(ep);
        OTSetBlocking(ep);
        OTInstallNotifier(ep, gNotifier, nil);
        OTUseSyncIdleEvents(ep, true);
    }
    return ep;
}

static OSStatus bind_ep(EndpointRef ep, int port, int qlen)
{
    InetAddress addr, ret;
    TBind req, rsp;
    OTInitInetAddress(&addr, (InetPort)port, 0);
    req.addr.buf = (UInt8 *)&addr;
    req.addr.len = sizeof(addr);
    req.addr.maxlen = sizeof(addr);
    req.qlen = (OTQLen)qlen;
    rsp.addr.buf = (UInt8 *)&ret;
    rsp.addr.len = 0;
    rsp.addr.maxlen = sizeof(ret);
    rsp.qlen = 0;
    return OTBind(ep, &req, &rsp);
}

int plat_os9_init(void)
{
    OSStatus err;
    if (gInited) return 0;
    err = InitOpenTransport();
    if (err != noErr) return -1;
    gNotifier = NewOTNotifyUPP(yield_notifier);
    gInited = 1;
    return 0;
}

void plat_init(void) { plat_os9_init(); }

double plat_now(void)
{
    return (double)TickCount() / 60.0;
}

unsigned long plat_rand32(void)
{
    static unsigned long s = 0;
    UnsignedWide us;
    if (s == 0) {
        Microseconds(&us);
        s = us.lo ^ (us.hi << 7) ^ (unsigned long)TickCount() ^ 0x9E3779B9UL;
        if (s == 0) s = 12345;
    }
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

static pascal void *thread_entry(void *param)
{
    tstart t = *(tstart *)param;
    free(param);
    t.fn(t.arg);
    if (t.done) *t.done = 1;
    return nil;
}

typedef struct { volatile int done; } thandle;

int plat_thread_start(plat_thread_fn fn, void *arg, void **handle)
{
    static ThreadEntryUPP entry = nil;
    ThreadID id;
    OSErr err;
    tstart *t = (tstart *)malloc(sizeof *t);
    thandle *h = nil;
    if (!entry) entry = NewThreadEntryUPP(thread_entry);
    t->fn = fn;
    t->arg = arg;
    t->done = nil;
    if (handle) {
        h = (thandle *)malloc(sizeof *h);
        h->done = 0;
        t->done = &h->done;
    }
    err = NewThread(kCooperativeThread, entry, t, THREAD_STACK, kCreateIfNeeded, nil, &id);
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
    OSStatus err;
    plat_udp *u;
    EndpointRef ep = open_ep(kUDPName, &err);
    InetHost g;
    UInt8 optbuf[kOTOptionHeaderSize + sizeof(TIPAddMulticast)];
    UInt8 rspbuf[kOTOptionHeaderSize + sizeof(TIPAddMulticast)];
    TOption *opt = (TOption *)optbuf;
    TIPAddMulticast *mc;
    TOptMgmt req, rsp;

    if (err != noErr) return nil;
    if (bind_ep(ep, port, 0) != noErr) {
        OTCloseProvider(ep);
        return nil;
    }
    if (str_to_ip(group, &g) == 0) {
        opt->level = INET_IP;
        opt->name = kIP_ADD_MEMBERSHIP;
        opt->len = kOTOptionHeaderSize + sizeof(TIPAddMulticast);
        opt->status = 0;
        mc = (TIPAddMulticast *)opt->value;
        mc->multicastGroupAddress = g;
        mc->interfaceAddress = 0;
        req.opt.buf = optbuf;
        req.opt.len = opt->len;
        req.opt.maxlen = sizeof(optbuf);
        req.flags = T_NEGOTIATE;
        rsp.opt.buf = rspbuf;
        rsp.opt.len = 0;
        rsp.opt.maxlen = sizeof(rspbuf);
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
    InetHost h;
    if (str_to_ip(ip, &h) != 0) return -1;
    OTInitInetAddress(&dst, (InetPort)port, h);
    ud.addr.buf = (UInt8 *)&dst;
    ud.addr.len = sizeof(dst);
    ud.addr.maxlen = sizeof(dst);
    ud.opt.buf = nil;
    ud.opt.len = 0;
    ud.opt.maxlen = 0;
    ud.udata.buf = (UInt8 *)buf;
    ud.udata.len = n;
    ud.udata.maxlen = n;
    OTSetBlocking(u->ep);
    return OTSndUData(u->ep, &ud) == noErr ? 0 : -1;
}

long plat_udp_recv(plat_udp *u, void *buf, size_t n, char *ip_out, int timeout_ms)
{
    double end = plat_now() + timeout_ms / 1000.0;
    InetAddress from;
    TUnitData ud;
    OTFlags flags;
    OSStatus err;

    OTSetNonBlocking(u->ep);
    for (;;) {
        ud.addr.buf = (UInt8 *)&from;
        ud.addr.len = 0;
        ud.addr.maxlen = sizeof(from);
        ud.opt.buf = nil;
        ud.opt.len = 0;
        ud.opt.maxlen = 0;
        ud.udata.buf = (UInt8 *)buf;
        ud.udata.len = 0;
        ud.udata.maxlen = n;
        flags = 0;
        err = OTRcvUData(u->ep, &ud, &flags);
        if (err == noErr) {
            ip_to_str(from.fHost, ip_out);
            return (long)ud.udata.len;
        }
        if (err == kOTLookErr) {
            if (OTLook(u->ep) == T_UDERR) OTRcvUDErr(u->ep, nil);
            continue;
        }
        if (err != kOTNoDataErr) return -1;
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
    OSStatus err;
    plat_listener *l;
    EndpointRef ep = open_ep(kTCPName, &err);
    if (err != noErr) return nil;
    /* queue length 1: one pending connection at a time keeps accepting simple */
    if (bind_ep(ep, port, 1) != noErr) {
        OTCloseProvider(ep);
        return nil;
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
    OSStatus err, e2;
    EndpointRef nep;
    plat_conn *c;

    for (;;) {
        OTSetNonBlocking(l->ep);
        call.addr.buf = (UInt8 *)&peer;
        call.addr.len = 0;
        call.addr.maxlen = sizeof(peer);
        call.opt.buf = nil;
        call.opt.len = 0;
        call.opt.maxlen = 0;
        call.udata.buf = nil;
        call.udata.len = 0;
        call.udata.maxlen = 0;
        call.sequence = 0;
        err = OTListen(l->ep, &call);
        if (err == noErr) break;
        if (err == kOTNoDataErr) {
            if (plat_now() >= end) return nil;
            YieldToAnyThread();
            continue;
        }
        if (err == kOTLookErr) {
            if (OTLook(l->ep) == T_DISCONNECT) OTRcvDisconnect(l->ep, nil);
            continue;
        }
        return nil;
    }

    nep = open_ep(kTCPName, &e2);
    if (e2 != noErr) return nil;
    if (OTBind(nep, nil, nil) != noErr) {
        OTCloseProvider(nep);
        return nil;
    }
    OTSetBlocking(l->ep);
    err = OTAccept(l->ep, nep, &call);
    if (err != noErr) {
        if (err == kOTLookErr && OTLook(l->ep) == T_DISCONNECT) OTRcvDisconnect(l->ep, nil);
        OTCloseProvider(nep);
        return nil;
    }
    ip_to_str(peer.fHost, ip_out);
    c = (plat_conn *)malloc(sizeof *c);
    c->ep = nep;
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
    TCall snd;
    InetHost h;
    OSStatus err;
    EndpointRef ep;
    plat_conn *c;

    (void)timeout_s;      /* OT applies its own TCP connect timeout */
    if (str_to_ip(ip, &h) != 0) return nil;
    ep = open_ep(kTCPName, &err);
    if (err != noErr) return nil;
    if (OTBind(ep, nil, nil) != noErr) {
        OTCloseProvider(ep);
        return nil;
    }
    OTInitInetAddress(&dst, (InetPort)port, h);
    snd.addr.buf = (UInt8 *)&dst;
    snd.addr.len = sizeof(dst);
    snd.addr.maxlen = sizeof(dst);
    snd.opt.buf = nil;
    snd.opt.len = 0;
    snd.opt.maxlen = 0;
    snd.udata.buf = nil;
    snd.udata.len = 0;
    snd.udata.maxlen = 0;
    snd.sequence = 0;
    OTSetBlocking(ep);
    err = OTConnect(ep, &snd, nil);
    if (err != noErr) {
        OTCloseProvider(ep);
        return nil;
    }
    c = (plat_conn *)malloc(sizeof *c);
    c->ep = ep;
    return c;
}

/* One poll step shared by read paths. */
static long conn_read_poll(plat_conn *c, void *buf, size_t n, double deadline)
{
    OTFlags flags;
    OTResult r;

    OTSetNonBlocking(c->ep);
    for (;;) {
        flags = 0;
        r = OTRcv(c->ep, buf, n, &flags);
        if (r > 0) return (long)r;
        if (r == kOTNoDataErr || r == 0) {
            if (plat_now() >= deadline) return -1;
            YieldToAnyThread();
            continue;
        }
        if (r == kOTLookErr) {
            switch (OTLook(c->ep)) {
            case T_DISCONNECT:
                OTRcvDisconnect(c->ep, nil);
                return 0;
            case T_ORDREL:
                OTRcvOrderlyDisconnect(c->ep);
                return 0;
            default:
                continue;
            }
        }
        return -1;
    }
}

long plat_conn_read(plat_conn *c, void *buf, size_t n)
{
    return conn_read_poll(c, buf, n, plat_now() + READ_TIMEOUT_S);
}

long plat_conn_write(plat_conn *c, const void *buf, size_t n)
{
    OTResult r;
    OTSetBlocking(c->ep);
    for (;;) {
        r = OTSnd(c->ep, (void *)buf, n, 0);
        if (r >= 0) return (long)r;
        if (r == kOTFlowErr) {
            YieldToAnyThread();
            continue;
        }
        return -1;
    }
}

int plat_conn_peek(plat_conn *c)
{
    (void)c;
    return 256;      /* no TLS on Mac OS 9: always plain HTTP */
}

int plat_conn_fd(plat_conn *c)
{
    (void)c;
    return -1;
}

void plat_conn_close(plat_conn *c)
{
    char scratch[256];
    double end;
    if (!c) return;
    OTSetBlocking(c->ep);
    if (OTSndOrderlyDisconnect(c->ep) == noErr) {
        /* let the peer read everything we sent before the stack tears the connection down */
        end = plat_now() + 2.0;
        while (conn_read_poll(c, scratch, sizeof scratch, end) > 0) {
        }
    }
    OTCloseProvider(c->ep);
    free(c);
}

/* ------------------------------------------------------------------ files */

static void to_pascal(const char *s, Str255 p)
{
    size_t n = strlen(s);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

static OSErr spec_for(const char *path, FSSpec *spec)
{
    Str255 p;
    to_pascal(path, p);
    return FSMakeFSSpec(0, 0, p, spec);
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
    *type = '????';
    *creator = '????';
    if (n > 4 && (name[n - 4] == '.') && (name[n - 3] == 't' || name[n - 3] == 'T')
        && (name[n - 2] == 'x' || name[n - 2] == 'X') && (name[n - 1] == 't' || name[n - 1] == 'T')) {
        *type = 'TEXT';
        *creator = 'ttxt';
    }
}

plat_file *plat_file_create_unique(const char *dir, const char *name, char *path_out, size_t ps)
{
    char first[256], cand[256], num[16];
    FSSpec spec;
    OSErr err;
    short ref;
    OSType type, creator;
    int n;
    size_t dl = strlen(dir);
    plat_file *f;

    hfs_name(name, first);
    type_for(first, &type, &creator);
    for (n = 0; n < 200; n++) {
        char nm[64];
        if (n == 0) {
            strcpy(nm, first);
        } else {
            /* "name 2.ext": rebuild with a suffix, keeping within 31 characters */
            char base[64], ext[40];
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
        if (dl + strlen(nm) + 2 > sizeof cand) return nil;
        strcpy(cand, dir);
        if (dl && cand[dl - 1] != ':') strcat(cand, ":");
        strcat(cand, nm);
        err = spec_for(cand, &spec);
        if (err == noErr) continue;           /* already exists */
        if (err != fnfErr) return nil;
        err = FSpCreate(&spec, creator, type, smSystemScript);
        if (err != noErr) return nil;
        err = FSpOpenDF(&spec, fsWrPerm, &ref);
        if (err != noErr) {
            FSpDelete(&spec);
            return nil;
        }
        strncpy(path_out, cand, ps - 1);
        path_out[ps - 1] = 0;
        f = (plat_file *)malloc(sizeof *f);
        f->ref = ref;
        return f;
    }
    return nil;
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
    FSSpec spec;
    if (spec_for(path, &spec) == noErr) FSpDelete(&spec);
}

plat_file *plat_file_open_read(const char *path)
{
    FSSpec spec;
    short ref;
    plat_file *f;
    if (spec_for(path, &spec) != noErr) return nil;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return nil;
    f = (plat_file *)malloc(sizeof *f);
    f->ref = ref;
    return f;
}

long plat_file_read(plat_file *f, void *buf, size_t n)
{
    long count = (long)n;
    OSErr err = FSRead(f->ref, &count, buf);
    if (err == noErr || (err == eofErr)) return count;     /* count is 0 at end of file */
    return -1;
}

int plat_file_size(const char *path, long long *size)
{
    FSSpec spec;
    short ref;
    long eof;
    if (spec_for(path, &spec) != noErr) return -1;
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

static int dir_to_path(short vRef, long dirID, char *out, int size)
{
    CInfoPBRec pb;
    Str255 name;
    char tmp[512];
    long id = dirID;
    int len = 0, guard = 0;

    tmp[0] = 0;
    for (;;) {
        OSErr err;
        int nl;
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = vRef;
        pb.dirInfo.ioFDirIndex = -1;
        pb.dirInfo.ioDrDirID = id;
        err = PBGetCatInfoSync(&pb);
        if (err != noErr) return -1;
        nl = name[0];
        if (len + nl + 1 >= (int)sizeof tmp) return -1;
        memmove(tmp + nl + 1, tmp, (size_t)len + 1);       /* prepend "name:" */
        memcpy(tmp, name + 1, (size_t)nl);
        tmp[nl] = ':';
        len += nl + 1;
        if (pb.dirInfo.ioDrDirID == fsRtDirID) break;       /* reached the volume root */
        id = pb.dirInfo.ioDrParID;
        if (++guard > 64) return -1;
    }
    if (len + 1 > size) return -1;
    memcpy(out, tmp, (size_t)len + 1);
    return 0;
}

int plat_os9_desktop_path(char *out, int size)
{
    short vRef;
    long dirID;
    if (FindFolder(kOnSystemDisk, kDesktopFolderType, kCreateFolder, &vRef, &dirID) != noErr) return -1;
    return dir_to_path(vRef, dirID, out, size);
}

int plat_os9_spec_to_path(const FSSpec *spec, char *out, int size)
{
    int len;
    if (dir_to_path(spec->vRefNum, spec->parID, out, size) != 0) return -1;
    len = (int)strlen(out);
    if (len + spec->name[0] + 1 > size) return -1;
    memcpy(out + len, spec->name + 1, spec->name[0]);
    out[len + spec->name[0]] = 0;
    return 0;
}

int plat_os9_local_ip(char *out, int size)
{
    InetInterfaceInfo info;
    if (size < 16) return -1;
    if (OTInetGetInterfaceInfo(&info, kDefaultInetInterface) != noErr) return -1;
    ip_to_str(info.fAddress, out);
    return 0;
}
