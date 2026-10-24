/* LocalSend for Mac OS 9 (Carbon: runs on Mac OS 9.1+ with CarbonLib, and on Mac OS X PowerPC).
 * One window drawn by hand with QuickDraw, like Quackify Classic. The protocol core runs in cooperative Thread Manager
 * threads; the event loop yields to them on every pass. Threads only set flags: all drawing happens here.
 * This version receives files; sending comes later. */
#include <Carbon.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls.h"
#include "ls_plat.h"
#include "ot_decl.h"
#include "plat_carbon.h"

#define APP_VERSION "0.1.1"
#define WIN_W 560
#define WIN_H 400
#define ROW_H 18

/* ---------------------------------------------------------------- state */

static WindowRef gWin = NULL;
static ls_ctx *gCore = NULL;
static int gQuit = 0;

static ls_peer gPeers[LS_MAX_PEERS];
static int gNPeers = 0;
static char gAlias[96];
static char gDir[48];
static char gIp[24];
static char gStatus[160];

/* shared with the core's threads */
static volatile long gPct = 0;
static volatile int gDirtyAll = 1;
static volatile int gDirtyBar = 0;
static volatile unsigned long gLastProgress = 0;
static struct {
    volatile int pending;
    volatile int accept;
    char from[64];
    char first[256];
    int count;
    long long size;
} gReq;

/* ------------------------------------------------------------------ utils */

static void to_pascal(const char *s, Str255 p)
{
    size_t n = strlen(s);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

/* UTF-8 from other devices, shown in MacRoman: ASCII stays, every other character becomes "?". */
static void to_display(const char *in, char *out, size_t os)
{
    size_t i = 0;
    while (*in && i + 1 < os) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x80) {
            out[i++] = (char)c;
            in++;
        } else {
            out[i++] = '?';
            in++;
            while (((unsigned char)*in & 0xC0) == 0x80) in++;
        }
    }
    out[i] = 0;
}

static int width_of(const char *s)
{
    Str255 p;
    to_pascal(s, p);
    return StringWidth(p);
}

/* Text at (x, baseline), cut with "..." to fit maxw pixels. */
static void draw_text(int x, int y, const char *text, int maxw)
{
    char shown[256];
    Str255 p;
    strncpy(shown, text, sizeof shown - 4);
    shown[sizeof shown - 4] = 0;
    if (width_of(shown) > maxw) {
        size_t n = strlen(shown);
        while (n > 0) {
            shown[n - 1] = 0;
            strcat(shown, "...");
            if (width_of(shown) <= maxw) break;
            shown[n - 1 > 0 ? n - 1 : 0] = 0;
            n--;
        }
    }
    to_pascal(shown, p);
    MoveTo((short)x, (short)y);
    DrawString(p);
}

static void set_color(unsigned short r, unsigned short g, unsigned short b)
{
    RGBColor c;
    c.red = r;
    c.green = g;
    c.blue = b;
    RGBForeColor(&c);
}
static void ink(void) { set_color(0, 0, 0); }
static void grey(void) { set_color(0x5555, 0x5555, 0x5555); }
static void teal(void) { set_color(0x0000, 0x7F00, 0x8000); }
static void red(void) { set_color(0xCCCC, 0, 0); }

static Rect make_rect(int l, int t, int r, int b)
{
    Rect rect;
    rect.left = (short)l;
    rect.top = (short)t;
    rect.right = (short)r;
    rect.bottom = (short)b;
    return rect;
}

static int inside(const Rect *r, Point p)
{
    return p.h >= r->left && p.h < r->right && p.v >= r->top && p.v < r->bottom;
}

static void invalidate(void)
{
    gDirtyAll = 1;
}

static void say(const char *text)
{
    strncpy(gStatus, text, sizeof gStatus - 1);
    gStatus[sizeof gStatus - 1] = 0;
    gDirtyAll = 1;
}

/* ------------------------------------------------------- core callbacks */

static void cb_peer(void *ud, const ls_peer *p)
{
    (void)ud;
    (void)p;
    gDirtyAll = 1;
}

static void cb_log(void *ud, const char *msg)
{
    (void)ud;
    say(msg);
}

static void cb_progress(void *ud, const char *name, long long done, long long total, int sending)
{
    char t[160], shown[96];
    long pct = 0;
    (void)ud;
    if (total > 0) pct = (long)((done * 100) / total);
    gPct = pct;
    gLastProgress = TickCount();
    to_display(name, shown, sizeof shown);
    sprintf(t, "%s %.80s: %ld%%", sending ? "Sending" : "Receiving", shown, pct);
    strncpy(gStatus, t, sizeof gStatus - 1);
    gStatus[sizeof gStatus - 1] = 0;
    gDirtyBar = 1;
}

static void cb_received(void *ud, const char *path)
{
    char t[160], shown[96];
    (void)ud;
    to_display(plat_basename(path), shown, sizeof shown);
    sprintf(t, "Received %.100s", shown);
    gPct = 0;
    gLastProgress = 0;
    say(t);
}

static int cb_incoming(void *ud, const char *from, int n, const ls_file *files)
{
    (void)ud;
    to_display(from, gReq.from, sizeof gReq.from);
    to_display(n > 0 ? files[0].name : "", gReq.first, sizeof gReq.first);
    gReq.count = n;
    gReq.size = n > 0 ? files[0].size : 0;
    gReq.accept = 0;
    gReq.pending = 1;
    gDirtyAll = 1;
    while (gReq.pending) YieldToAnyThread();     /* the event loop answers */
    return gReq.accept;
}

/* ---------------------------------------------------------------- layout */

static Rect list_rect(void) { return make_rect(12, 78, WIN_W - 12, 78 + ROW_H * 8 + 2); }
static Rect refresh_btn(void) { return make_rect(12, 236, 92, 260); }
static Rect bar_rect(void) { return make_rect(12, 276, WIN_W - 12, 290); }
static Rect accept_btn(void) { return make_rect(300, 252, 380, 278); }
static Rect decline_btn(void) { return make_rect(392, 252, 472, 278); }

/* --------------------------------------------------------------- drawing */

static void draw_button(const Rect *r, const char *label, int isdefault)
{
    Rect box = *r;
    int w;
    ink();
    EraseRoundRect(&box, 10, 10);
    FrameRoundRect(&box, 10, 10);
    if (isdefault) {
        Rect ring = *r;
        InsetRect(&ring, -3, -3);
        PenSize(2, 2);
        FrameRoundRect(&ring, 13, 13);
        PenNormal();
    }
    w = width_of(label);
    draw_text((r->left + r->right - w) / 2, r->top + 17, label, r->right - r->left - 4);
}

static void draw_bar(void)
{
    Rect bar = bar_rect(), fill, status;
    int span;
    EraseRect(&bar);
    ink();
    FrameRect(&bar);
    span = bar.right - bar.left - 2;
    if (gPct > 0) {
        fill = make_rect(bar.left + 1, bar.top + 1, bar.left + 1 + (int)((span * gPct) / 100), bar.bottom - 1);
        teal();
        PaintRect(&fill);
        ink();
    }
    status = make_rect(0, 296, WIN_W, 320);
    EraseRect(&status);
    ink();
    TextFace(normal);
    draw_text(12, 312, gStatus, WIN_W - 24);
}

static void draw_prompt(void)
{
    Rect box = make_rect(60, 96, WIN_W - 60, 296);
    Rect shadow = box;
    char line[200], name[200], sz[48];
    OffsetRect(&shadow, 3, 3);
    set_color(0x7777, 0x7777, 0x7777);
    PaintRect(&shadow);
    set_color(0xFFFF, 0xFFFF, 0xFFFF);
    PaintRect(&box);
    ink();
    PenSize(2, 2);
    FrameRect(&box);
    PenNormal();
    TextFace(bold);
    draw_text(80, 126, "Incoming file", 400);
    TextFace(normal);
    sprintf(line, "%.60s wants to send you:", gReq.from);
    draw_text(80, 152, line, 400);
    strncpy(name, gReq.first, sizeof name - 1);
    name[sizeof name - 1] = 0;
    if (gReq.size >= 1024 * 1024) sprintf(sz, "%ld.%ld MB", (long)(gReq.size / (1024 * 1024)), (long)((gReq.size / 104858) % 10));
    else sprintf(sz, "%ld KB", (long)((gReq.size + 1023) / 1024));
    sprintf(line, "%s (%s)", name, sz);
    draw_text(92, 176, line, 380);
    if (gReq.count > 1) {
        sprintf(line, "and %d more file%s", gReq.count - 1, gReq.count > 2 ? "s" : "");
        draw_text(92, 196, line, 380);
    }
    {
        Rect ab = accept_btn(), db = decline_btn();
        draw_button(&ab, "Accept", 1);
        draw_button(&db, "Decline", 0);
    }
}

static void draw(void)
{
    Rect all = make_rect(0, 0, WIN_W, WIN_H);
    Rect list = list_rect();
    char buf[200], a[96], ip[40];
    int i;

    SetPortWindowPort(gWin);
    EraseRect(&all);
    TextFont(0);
    TextSize(12);
    TextFace(normal);
    ink();

    TextSize(20);
    TextFace(bold);
    teal();
    draw_text(12, 32, "LocalSend", 200);
    ink();
    TextSize(12);
    TextFace(normal);
    draw_text(122, 32, "for Mac OS 9", 200);
    grey();
    draw_text(WIN_W - 130, 32, "version " APP_VERSION, 118);
    ink();

    sprintf(buf, "This Mac appears in LocalSend as \"%.60s\".", gAlias);
    draw_text(12, 56, buf, WIN_W - 24);
    draw_text(12, 74, "Nearby devices", 200);

    FrameRect(&list);
    for (i = 0; i < gNPeers && i < 8; i++) {
        int top = list.top + 1 + i * ROW_H;
        to_display(gPeers[i].alias, a, sizeof a);
        sprintf(ip, "%s%s", gPeers[i].ip, strcmp(gPeers[i].protocol, "http") == 0 ? "" : "  (encrypted)");
        draw_text(list.left + 8, top + 13, a, 300);
        draw_text(list.right - 8 - width_of(ip), top + 13, ip, 200);
    }
    if (gNPeers == 0) {
        grey();
        draw_text(list.left + 10, list.top + 24, "No other devices found yet. Open LocalSend on another device.", 500);
        ink();
    }

    {
        Rect rb = refresh_btn();
        draw_button(&rb, "Refresh", 0);
    }
    grey();
    draw_text(104, 252, "To send a file here: pick this Mac in LocalSend on the other device.", WIN_W - 116);
    ink();

    draw_bar();

    grey();
    sprintf(buf, "This Mac's address: %s.   Received files go to the Desktop.", gIp[0] ? gIp : "none (is TCP/IP on?)");
    draw_text(12, 342, buf, WIN_W - 24);
    draw_text(12, 360, "This version receives files. Sending from this Mac comes later.", WIN_W - 24);
    ink();

    if (gReq.pending) draw_prompt();
}

/* --------------------------------------------------- system integration */

static int on_mac_os_x(void)
{
    long version = 0;
    return Gestalt('sysv', &version) == noErr && version >= 0x1000;
}

static pascal OSErr on_quit_event(const AppleEvent *ev, AppleEvent *reply, long refcon)
{
    (void)ev;
    (void)reply;
    (void)refcon;
    gQuit = 1;
    return noErr;
}

static void install_system_handlers(void)
{
    AEInstallEventHandler('aevt', 'quit', NewAEEventHandlerUPP(on_quit_event), 0, false);
    if (on_mac_os_x()) return;      /* Mac OS X makes the Apple and application menus itself */
    {
        MenuHandle apple = NewMenu(128, "\p\x14");
        MenuHandle file = NewMenu(129, "\pFile");
        AppendMenu(apple, "\pAbout LocalSend...");
        InsertMenu(apple, 0);
        AppendMenu(file, "\pRefresh/R;(-;Quit/Q");
        InsertMenu(file, 0);
        DrawMenuBar();
    }
}

static void refresh_now(void)
{
    if (gCore) ls_announce(gCore);
    say("Looking for devices...");
}

static void do_menu(long chosen)
{
    int menu = (int)(chosen >> 16), item = (int)(chosen & 0xFFFF);
    if (menu == 129 && item == 1) refresh_now();
    else if (menu == 129 && item == 3) gQuit = 1;
    else if (menu == 128 && item == 1)
        say("LocalSend for Mac OS 9 " APP_VERSION ". Compatible with LocalSend (localsend.org), not affiliated with it.");
    HiliteMenu(0);
}

static void on_key(char c, short modifiers)
{
    if (modifiers & cmdKey) {
        if (c == 'q' || c == 'Q') gQuit = 1;
        else if (c == '.' && gReq.pending) { gReq.accept = 0; gReq.pending = 0; invalidate(); }
        else if ((c == 'r' || c == 'R') && !gReq.pending) refresh_now();
        return;
    }
    if (gReq.pending) {
        if (c == '\r' || c == 3) { gReq.accept = 1; gReq.pending = 0; invalidate(); }
        else if (c == 27) { gReq.accept = 0; gReq.pending = 0; invalidate(); }
    }
}

static void on_click(EventRecord *ev)
{
    Point p = ev->where;
    Rect r;
    GlobalToLocal(&p);
    if (gReq.pending) {
        r = accept_btn();
        if (inside(&r, p)) { gReq.accept = 1; gReq.pending = 0; invalidate(); return; }
        r = decline_btn();
        if (inside(&r, p)) { gReq.accept = 0; gReq.pending = 0; invalidate(); return; }
        return;
    }
    r = refresh_btn();
    if (inside(&r, p)) refresh_now();
}

static void refresh_peers(void)
{
    ls_peer fresh[LS_MAX_PEERS];
    int n, i, same;
    if (!gCore) return;
    n = ls_peer_list(gCore, fresh, LS_MAX_PEERS);
    same = (n == gNPeers);
    for (i = 0; same && i < n; i++)
        if (strcmp(fresh[i].fingerprint, gPeers[i].fingerprint) != 0 || strcmp(fresh[i].alias, gPeers[i].alias) != 0)
            same = 0;
    memcpy(gPeers, fresh, (size_t)n * sizeof(ls_peer));
    gNPeers = n;
    if (!same) gDirtyAll = 1;
}

static void pump(void)
{
    static unsigned long lastScan = 0;
    EventRecord ev;
    int busy = (gLastProgress != 0 && TickCount() - gLastProgress < 120) || gReq.pending;
    int got;

    YieldToAnyThread();
    got = WaitNextEvent(everyEvent, &ev, busy ? 0 : 2, NULL);

    if (TickCount() - lastScan > 60) {
        lastScan = TickCount();
        refresh_peers();
    }
    if (gDirtyAll) {
        Rect all = make_rect(0, 0, WIN_W, WIN_H);
        gDirtyAll = 0;
        gDirtyBar = 0;
        SetPortWindowPort(gWin);
        InvalWindowRect(gWin, &all);
    } else if (gDirtyBar) {
        Rect bar = make_rect(0, 270, WIN_W, 322);
        gDirtyBar = 0;
        SetPortWindowPort(gWin);
        InvalWindowRect(gWin, &bar);
    }
    if (!got) return;

    switch (ev.what) {
    case updateEvt:
        BeginUpdate(gWin);
        draw();
        EndUpdate(gWin);
        break;
    case kHighLevelEvent:
        AEProcessAppleEvent(&ev);
        break;
    case keyDown:
    case autoKey:
        if ((ev.modifiers & cmdKey) && !on_mac_os_x()) {
            long chosen = MenuKey((char)(ev.message & charCodeMask));
            if ((chosen >> 16) != 0) { do_menu(chosen); break; }
        }
        on_key((char)(ev.message & charCodeMask), ev.modifiers);
        break;
    case mouseDown: {
        WindowRef hit;
        short part = FindWindow(ev.where, &hit);
        if (part == inMenuBar) do_menu(MenuSelect(ev.where));
        else if (part == inGoAway && TrackGoAway(hit, ev.where)) gQuit = 1;
        else if (part == inDrag) { Rect limit = make_rect(0, 0, 4000, 4000); DragWindow(hit, ev.where, &limit); }
        else if (part == inContent) { if (hit != FrontWindow()) SelectWindow(hit); else on_click(&ev); }
        break;
    }
    default:
        break;
    }
}

static void start_core(void)
{
    ls_callbacks cb;
    Handle h;

    strcpy(gAlias, "Mac OS 9");
    h = GetResource('STR ', -16413);            /* the Macintosh name from Sharing Setup */
    if (h && **(unsigned char **)h > 0) {
        unsigned char n = **(unsigned char **)h;
        char name[64];
        if (n > 40) n = 40;
        memcpy(name, *(char **)h + 1, n);
        name[n] = 0;
        strcpy(gAlias, name);
    }
    if (strlen(gAlias) > 56) gAlias[56] = 0;
    strcat(gAlias, " (OS 9)");

    memset(&cb, 0, sizeof cb);
    cb.peer = cb_peer;
    cb.incoming = cb_incoming;
    cb.progress = cb_progress;
    cb.received = cb_received;
    cb.log = cb_log;

    if (plat_carbon_desktop_dir(gDir, (int)sizeof gDir) != 0) {
        say("Cannot find the Desktop folder.");
        return;
    }
    gIp[0] = 0;
    plat_carbon_local_ip(gIp, (int)sizeof gIp);
    gCore = ls_start(gAlias, gDir, NULL, LS_PORT, &cb);
    if (!gCore) say("Could not start. Is TCP/IP on, and is another LocalSend running?");
    else say("Ready. Waiting for devices...");
}

int main(void)
{
    Rect r;
    int i;

    InitCursor();
    r = make_rect(60, 60 + 22, 60 + WIN_W, 60 + 22 + WIN_H);
    gWin = NewCWindow(0, &r, "\pLocalSend", true, documentProc, (WindowPtr)-1, true, 0);
    SetPortWindowPort(gWin);
    install_system_handlers();
    gStatus[0] = 0;
    start_core();
    for (i = 0; i < 3; i++) pump();
    while (!gQuit) pump();
    if (gCore) ls_stop(gCore);
    return 0;
}
