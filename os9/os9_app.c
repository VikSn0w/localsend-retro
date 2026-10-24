/* LocalSend for Mac OS 9 (PowerPC, Carbon-less Toolbox application).
 * Single window: device list, Send File, Refresh, progress bar, status line.
 * The protocol core runs in cooperative Thread Manager threads; the event loop
 * yields to them on every pass. Threads only set flags - all drawing happens here. */
#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Controls.h>
#include <Lists.h>
#include <ToolUtils.h>
#include <Devices.h>
#include <Threads.h>
#include <StandardFile.h>
#include <Resources.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Files.h>

#include <string.h>
#include <stdio.h>

#include "ls.h"
#include "ls_plat.h"
#include "plat_os9.h"

#define kApplePK 128
#define kFilePK  129
#define kEditPK  130
#define kAboutAlert 128
#define kIncomingAlert 129
#define kSizeResID 128

enum { mFileSend = 1, mFileRefresh = 2, mFileQuit = 4 };

static ls_ctx *gCore = NULL;
static WindowPtr gWin = NULL;
static ListHandle gList = NULL;
static ControlHandle gSendBtn = NULL, gRefreshBtn = NULL;
static int gQuit = 0;
static char gDesktop[256];
static char gLocalIp[24];
static ls_peer gPeers[LS_MAX_PEERS];
static int gNPeers = 0;
static ModalFilterUPP gFilter = NULL;

/* state shared with the core's threads */
static char gStatus[100];
static volatile long gPct = 0;
static volatile int gDirty = 1;
static volatile int gSending = 0;
static volatile int gSendResult = 0;
static volatile int gSendDone = 0;

static struct {
    volatile int pending;
    volatile int accept;
    char from[64];
    char first[256];
    int count;
} gReq;

static struct {
    ls_peer peer;
    char path[512];
} gJob;

/* ------------------------------------------------------------------ utils */

static void to_pascal(const char *s, Str255 p)
{
    size_t n = strlen(s);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

static void set_status(const char *s)
{
    strncpy(gStatus, s, sizeof gStatus - 1);
    gStatus[sizeof gStatus - 1] = 0;
    gDirty = 1;
}

static void draw_text(short x, short y, const char *s)
{
    Str255 p;
    to_pascal(s, p);
    MoveTo(x, y);
    DrawString(p);
}

/* ------------------------------------------------------- core callbacks */

static void cb_peer(void *ud, const ls_peer *p)
{
    (void)ud;
    (void)p;
    gDirty = 1;
}

static void cb_log(void *ud, const char *msg)
{
    (void)ud;
    set_status(msg);
}

static void cb_progress(void *ud, const char *name, long long done, long long total, int sending)
{
    char t[100];
    long pct = 0;
    (void)ud;
    if (total > 0) pct = (long)((done * 100) / total);
    gPct = pct;
    sprintf(t, "%s %.60s: %ld%%", sending ? "Sending" : "Receiving", name, pct);
    set_status(t);
}

static void cb_received(void *ud, const char *path)
{
    char t[100];
    (void)ud;
    sprintf(t, "Received %.80s", plat_basename(path));
    gPct = 0;
    set_status(t);
}

static int cb_incoming(void *ud, const char *from, int n, const ls_file *files)
{
    (void)ud;
    strncpy(gReq.from, from, sizeof gReq.from - 1);
    gReq.from[sizeof gReq.from - 1] = 0;
    strncpy(gReq.first, n > 0 ? files[0].name : "", sizeof gReq.first - 1);
    gReq.first[sizeof gReq.first - 1] = 0;
    gReq.count = n;
    gReq.accept = 0;
    gReq.pending = 1;
    while (gReq.pending) YieldToAnyThread();     /* the event loop answers */
    return gReq.accept;
}

/* -------------------------------------------------------------------- UI */

static pascal Boolean alert_filter(DialogPtr d, EventRecord *e, short *item)
{
    (void)d;
    (void)e;
    (void)item;
    YieldToAnyThread();
    return false;
}

static void ask_incoming(void)
{
    Str255 a, b;
    char msg[320];
    short hit;
    if (gReq.count > 1)
        sprintf(msg, "%.200s and %d more files", gReq.first, gReq.count - 1);
    else
        sprintf(msg, "%.250s", gReq.first);
    to_pascal(gReq.from, a);
    to_pascal(msg, b);
    ParamText(a, b, "\p", "\p");
    SetCursor(&qd.arrow);
    hit = Alert(kIncomingAlert, gFilter);
    gReq.accept = (hit == 1);
    gReq.pending = 0;
    gDirty = 1;
}

static int selected_row(void)
{
    Cell c;
    c.h = 0;
    c.v = 0;
    if (gList && LGetSelect(true, &c, gList)) return c.v;
    return -1;
}

static void update_buttons(void)
{
    int row = selected_row();
    int can = (row >= 0 && row < gNPeers && strcmp(gPeers[row].protocol, "http") == 0 && !gSending);
    HiliteControl(gSendBtn, can ? 0 : 255);
}

static void rebuild_list(void)
{
    ls_peer fresh[LS_MAX_PEERS];
    int n, i, same;
    char selfp[72];
    int row = selected_row();

    if (!gCore) return;
    n = ls_peer_list(gCore, fresh, LS_MAX_PEERS);
    same = (n == gNPeers);
    for (i = 0; same && i < n; i++)
        if (strcmp(fresh[i].fingerprint, gPeers[i].fingerprint) != 0 ||
            strcmp(fresh[i].alias, gPeers[i].alias) != 0 ||
            strcmp(fresh[i].protocol, gPeers[i].protocol) != 0)
            same = 0;
    memcpy(gPeers, fresh, (size_t)n * sizeof(ls_peer));
    if (same) { gNPeers = n; return; }

    selfp[0] = 0;
    if (row >= 0 && row < gNPeers) strncpy(selfp, gPeers[row].fingerprint, sizeof selfp - 1);
    selfp[sizeof selfp - 1] = 0;
    gNPeers = n;

    LSetDrawingMode(false, gList);
    LDelRow(0, 0, gList);
    if (n > 0) LAddRow(n, 0, gList);
    for (i = 0; i < n; i++) {
        char t[200];
        Cell c;
        c.h = 0;
        c.v = i;
        sprintf(t, "%.60s   %s%s", gPeers[i].alias, gPeers[i].ip,
                strcmp(gPeers[i].protocol, "http") == 0 ? "" : "   (HTTPS only)");
        LSetCell(t, (short)strlen(t), c, gList);
        if (selfp[0] && strcmp(selfp, gPeers[i].fingerprint) == 0) LSetSelect(true, c, gList);
    }
    LSetDrawingMode(true, gList);
    gDirty = 1;
    if (gWin) {
        SetPort((GrafPtr)gWin);
        InvalRect(&gWin->portRect);
    }
}

/* the parts that change while a transfer runs: progress bar and status line */
static void draw_dynamic(void)
{
    Rect bar, fill, st;
    char t[160];

    SetPort((GrafPtr)gWin);
    TextFont(geneva);
    TextSize(9);
    SetRect(&st, 12, 234, 348, 250);
    EraseRect(&st);
    draw_text(12, 246, gStatus);

    SetRect(&bar, 12, 218, 348, 230);
    EraseRect(&bar);
    FrameRect(&bar);
    fill = bar;
    InsetRect(&fill, 1, 1);
    fill.right = (short)(fill.left + ((fill.right - fill.left) * gPct) / 100);
    if (gPct > 0) PaintRect(&fill);

    SetRect(&st, 12, 258, 348, 276);
    EraseRect(&st);
    sprintf(t, "Saving received files to the Desktop.   This Mac: %s", gLocalIp[0] ? gLocalIp : "?");
    draw_text(12, 270, t);
    update_buttons();
    gDirty = 0;
}

static void draw_window(void)
{
    Rect lr;
    SetPort((GrafPtr)gWin);
    EraseRect(&gWin->portRect);
    TextFont(systemFont);
    TextSize(12);
    draw_text(12, 20, "Nearby devices");
    draw_dynamic();
    if (gList) {
        LUpdate(gWin->visRgn, gList);
        lr = (*gList)->rView;
        InsetRect(&lr, -1, -1);
        FrameRect(&lr);
    }
    DrawControls(gWin);
}

static void do_send(void)
{
    StandardFileReply reply;
    int row = selected_row();
    if (row < 0 || row >= gNPeers || gSending) return;
    if (strcmp(gPeers[row].protocol, "http") != 0) {
        set_status("That device needs HTTPS. Turn encryption off in its settings.");
        return;
    }
    StandardGetFile(nil, -1, nil, &reply);
    if (!reply.sfGood) return;
    if (plat_os9_spec_to_path(&reply.sfFile, gJob.path, (int)sizeof gJob.path) != 0) {
        set_status("Could not read that file's location.");
        return;
    }
    gJob.peer = gPeers[row];
    gSending = 1;
    gSendDone = 0;
    gPct = 0;
    HiliteControl(gSendBtn, 255);
    set_status("Contacting device...");
}

static void send_thread(void *arg)
{
    const char *paths[1];
    (void)arg;
    paths[0] = gJob.path;
    gSendResult = ls_send(gCore, &gJob.peer, 1, paths);
    gSendDone = 1;
}

static void start_send_thread(void)
{
    if (plat_thread_start(send_thread, NULL, NULL) != 0) {
        gSending = 0;
        set_status("Could not start the transfer.");
    }
}

static void do_about(void)
{
    Alert(kAboutAlert, gFilter);
}

static void do_menu(long choice)
{
    short menu = HiWord(choice), item = LoWord(choice);
    if (menu == kApplePK) {
        if (item == 1) {
            do_about();
        } else {
            Str255 name;
            GetMenuItemText(GetMenuHandle(kApplePK), item, name);
            OpenDeskAcc(name);
        }
    } else if (menu == kFilePK) {
        switch (item) {
        case mFileSend:
            do_send();
            if (gSending) start_send_thread();
            break;
        case mFileRefresh:
            if (gCore) ls_announce(gCore);
            set_status("Looking for devices...");
            break;
        case mFileQuit:
            gQuit = 1;
            break;
        }
    }
    HiliteMenu(0);
}

static void do_mouse_down(EventRecord *e)
{
    WindowPtr w;
    short part = FindWindow(e->where, &w);
    switch (part) {
    case inMenuBar:
        do_menu(MenuSelect(e->where));
        break;
    case inSysWindow:
        SystemClick(e, w);
        break;
    case inDrag:
        DragWindow(w, e->where, &qd.screenBits.bounds);
        break;
    case inGoAway:
        if (TrackGoAway(w, e->where)) gQuit = 1;
        break;
    case inContent:
        if (w != FrontWindow()) {
            SelectWindow(w);
        } else {
            Point pt = e->where;
            ControlHandle ctl;
            SetPort((GrafPtr)w);
            GlobalToLocal(&pt);
            if (FindControl(pt, w, &ctl)) {
                if (TrackControl(ctl, pt, nil)) {
                    if (ctl == gSendBtn) {
                        do_send();
                        if (gSending) start_send_thread();
                    } else if (ctl == gRefreshBtn) {
                        if (gCore) ls_announce(gCore);
                        set_status("Looking for devices...");
                    }
                }
            } else if (gList) {
                LClick(pt, e->modifiers, gList);
                update_buttons();
            }
        }
        break;
    }
}

static void do_event(EventRecord *e)
{
    switch (e->what) {
    case mouseDown:
        do_mouse_down(e);
        break;
    case keyDown:
    case autoKey:
        if (e->modifiers & cmdKey) {
            do_menu(MenuKey((char)(e->message & charCodeMask)));
        }
        break;
    case updateEvt:
        if ((WindowPtr)e->message == gWin) {
            BeginUpdate(gWin);
            draw_window();
            EndUpdate(gWin);
        }
        break;
    case activateEvt:
        if ((WindowPtr)e->message == gWin && gList)
            LActivate((e->modifiers & activeFlag) != 0, gList);
        break;
    }
}

static void init_toolbox(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(nil);
    InitCursor();
    MaxApplZone();
    MoreMasters();
    MoreMasters();
    MoreMasters();
    FlushEvents(everyEvent, 0);
}

static void build_menus(void)
{
    Handle mbar = GetNewMBar(128);
    MenuHandle apple;
    if (!mbar) return;
    SetMenuBar(mbar);
    apple = GetMenuHandle(kApplePK);
    if (apple) AppendResMenu(apple, 'DRVR');
    DrawMenuBar();
}

static void build_window(void)
{
    Rect r, lr, br;
    Point cs;
    Rect db;

    SetRect(&r, 40, 60, 40 + 360, 60 + 284);
    gWin = NewWindow(nil, &r, "\pLocalSend", true, documentProc, (WindowPtr)-1L, true, 0);
    SetPort((GrafPtr)gWin);

    SetRect(&lr, 12, 28, 332, 168);
    SetRect(&db, 0, 0, 1, 0);
    cs.h = 0;
    cs.v = 0;
    gList = LNew(&lr, &db, cs, 0, gWin, true, false, false, true);
    (*gList)->selFlags = lOnlyOne;

    SetRect(&br, 12, 180, 122, 202);
    gSendBtn = NewControl(gWin, &br, "\pSend File...", true, 0, 0, 1, pushButProc, 0);
    SetRect(&br, 132, 180, 222, 202);
    gRefreshBtn = NewControl(gWin, &br, "\pRefresh", true, 0, 0, 1, pushButProc, 0);
    HiliteControl(gSendBtn, 255);
}

static void start_core(void)
{
    ls_callbacks cb;
    char alias[80];
    Handle h;

    strcpy(alias, "Mac OS 9");
    h = GetResource('STR ', -16413);               /* the Macintosh name from Sharing Setup */
    if (h && **(unsigned char **)h > 0) {
        unsigned char n = **(unsigned char **)h;
        if (n > 60) n = 60;
        memcpy(alias, *(char **)h + 1, n);
        alias[n] = 0;
    }
    strcat(alias, " (OS 9)");

    memset(&cb, 0, sizeof cb);
    cb.peer = cb_peer;
    cb.incoming = cb_incoming;
    cb.progress = cb_progress;
    cb.received = cb_received;
    cb.log = cb_log;

    if (plat_os9_desktop_path(gDesktop, (int)sizeof gDesktop) != 0) {
        set_status("Cannot find the Desktop folder.");
        return;
    }
    plat_os9_local_ip(gLocalIp, (int)sizeof gLocalIp);
    gCore = ls_start(alias, gDesktop, NULL, LS_PORT, &cb);
    if (!gCore)
        set_status("Could not start. Is TCP/IP on, and another LocalSend running?");
    else
        set_status("Ready. Waiting for devices...");
}

int main(void)
{
    EventRecord ev;
    unsigned long lastScan = 0;

    init_toolbox();
    gFilter = NewModalFilterUPP(alert_filter);
    if (plat_os9_init() != 0) {
        StopAlert(kAboutAlert, nil);
        return 0;
    }
    gStatus[0] = 0;
    gLocalIp[0] = 0;
    build_menus();
    build_window();
    start_core();

    while (!gQuit) {
        YieldToAnyThread();
        if (WaitNextEvent(everyEvent, &ev, 2, nil)) do_event(&ev);

        if (gReq.pending) ask_incoming();
        if (gSendDone) {
            gSendDone = 0;
            gSending = 0;
            gPct = 0;
            if (gSendResult == 0) set_status("Sent.");
            else if (gSendResult == -3) set_status("The other device declined.");
            else if (gSendResult != -2) set_status("Sending failed.");
            update_buttons();
        }
        if (TickCount() - lastScan > 60) {
            lastScan = TickCount();
            rebuild_list();
        }
        if (gDirty && gWin) {
            GrafPtr save;
            GetPort(&save);
            draw_dynamic();
            SetPort(save);
        }
    }

    if (gCore) ls_stop(gCore);
    return 0;
}
