/* Cocoa front end. Written for Mac OS X 10.3 - 10.5 (Objective-C, manual
 * memory management, no blocks / GCD / ARC). Built entirely in code, no nib. */
#import "AppController.h"
#include <unistd.h>
#include "config.h"
#include "ls_update.h"

static AppController *gController = nil;

/* ---- C callbacks (background threads) -> main thread ------------------- */

static void cb_peer(void *ud, const ls_peer *p) { (void)ud; (void)p; }

static void cb_log(void *ud, const char *msg)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSLog(@"%s", msg);
    [gController performSelectorOnMainThread:@selector(setStatusText:)
                                  withObject:[NSString stringWithUTF8String:msg]
                               waitUntilDone:NO];
    [pool release];
}

static void cb_progress(void *ud, const char *name, long long done, long long total, int sending)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    double frac = total > 0 ? (double)done / (double)total : 0;
    NSString *s = [NSString stringWithFormat:@"%@ %s: %d%%", sending ? @"Sending" : @"Receiving",
                   name, (int)(frac * 100)];
    NSDictionary *d = [NSDictionary dictionaryWithObjectsAndKeys:
                       [NSNumber numberWithDouble:frac * 100], @"pct", s, @"text", nil];
    [gController performSelectorOnMainThread:@selector(setProgressInfo:)
                                  withObject:d waitUntilDone:NO];
    [pool release];
}

static void cb_received(void *ud, const char *path)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [gController performSelectorOnMainThread:@selector(setStatusText:)
                                  withObject:[NSString stringWithFormat:@"Received %s", path]
                               waitUntilDone:NO];
    [pool release];
}

static int cb_incoming(void *ud, const char *from, int n, const ls_file *files)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSMutableString *msg = [NSMutableString stringWithFormat:@"%s wants to send you:\n\n", from];
    NSMutableDictionary *req;
    int i, accept;
    for (i = 0; i < n && i < 8; i++)
        [msg appendFormat:@"%s (%.1f KB)\n", files[i].name, files[i].size / 1024.0];
    if (n > 8) [msg appendFormat:@"... and %d more\n", n - 8];
    req = [NSMutableDictionary dictionaryWithObject:msg forKey:@"message"];
    [gController performSelectorOnMainThread:@selector(askIncoming:)
                                  withObject:req waitUntilDone:YES];
    accept = [[req objectForKey:@"accept"] boolValue];
    [pool release];
    return accept;
}

static void dl_progress(void *ud, long long done, long long total)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    double frac = total > 0 ? (double)done / (double)total : 0;
    NSDictionary *d = [NSDictionary dictionaryWithObjectsAndKeys:
                       [NSNumber numberWithDouble:frac * 100], @"pct",
                       [NSString stringWithFormat:@"Downloading update: %d%%", (int)(frac * 100)], @"text", nil];
    [gController performSelectorOnMainThread:@selector(setProgressInfo:) withObject:d waitUntilDone:NO];
    [pool release];
}

/* ------------------------------------------------------------------------ */

@implementation AppController

- (id)init
{
    self = [super init];
    if (self) {
        NSString *saved;
        gController = self;
        saved = [[NSUserDefaults standardUserDefaults] stringForKey:@"downloadDir"];
        downloadDir = [(saved ? saved
                              : [NSHomeDirectory() stringByAppendingPathComponent:@"Downloads"]) retain];
    }
    return self;
}

- (NSString *)dataDir
{
    return [NSHomeDirectory() stringByAppendingPathComponent:
            @"Library/Application Support/LocalSendPPC"];
}

- (NSString *)appVersion
{
    NSString *v = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"];
    return v ? v : @"0";
}

- (NSString *)caPath
{
    return [[NSBundle mainBundle] pathForResource:@"cacert" ofType:@"pem"];
}

- (NSTextField *)labelAt:(NSRect)r text:(NSString *)t
{
    NSTextField *f = [[[NSTextField alloc] initWithFrame:r] autorelease];
    [f setStringValue:t];
    [f setBezeled:NO];
    [f setDrawsBackground:NO];
    [f setEditable:NO];
    [f setSelectable:NO];
    return f;
}

- (NSButton *)buttonAt:(NSRect)r title:(NSString *)t action:(SEL)a
{
    NSButton *b = [[[NSButton alloc] initWithFrame:r] autorelease];
    [b setTitle:t];
    [b setBezelStyle:NSRoundedBezelStyle];
    [b setTarget:self];
    [b setAction:a];
    return b;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    NSView *content;
    NSScrollView *scroll;
    NSTableColumn *c1, *c2, *c3;
    NSButton *folderButton, *refresh;
    NSTextField *lbl;
    ls_callbacks cb;
    NSString *host = [[NSProcessInfo processInfo] hostName];
    NSString *alias, *saved;

    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 450)
                                         styleMask:(NSTitledWindowMask | NSClosableWindowMask |
                                                    NSMiniaturizableWindowMask | NSResizableWindowMask)
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    [window setTitle:@"LocalSend for PowerPC"];
    [window setMinSize:NSMakeSize(440, 330)];
    content = [window contentView];

    /* device name, pinned to the top */
    defaultName = [(host ? [NSString stringWithFormat:@"%@ (PPC)", host] : @"PowerPC Mac") retain];
    saved = [[NSUserDefaults standardUserDefaults] stringForKey:@"deviceName"];
    alias = (saved && [saved length]) ? saved : defaultName;
    lbl = [self labelAt:NSMakeRect(20, 418, 90, 17) text:@"Device name:"];
    [lbl setAutoresizingMask:NSViewMinYMargin];
    [content addSubview:lbl];
    nameField = [[[NSTextField alloc] initWithFrame:NSMakeRect(112, 414, 388, 22)] autorelease];
    [nameField setStringValue:alias];
    [nameField setTarget:self];
    [nameField setAction:@selector(nameChanged:)];
    [nameField setDelegate:self];
    [nameField setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [content addSubview:nameField];

    lbl = [self labelAt:NSMakeRect(20, 384, 300, 17) text:@"Nearby devices"];
    [lbl setAutoresizingMask:NSViewMinYMargin];
    [content addSubview:lbl];

    scroll = [[[NSScrollView alloc] initWithFrame:NSMakeRect(20, 150, 480, 228)] autorelease];
    [scroll setHasVerticalScroller:YES];
    [scroll setBorderType:NSBezelBorder];
    [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    table = [[[NSTableView alloc] initWithFrame:[scroll bounds]] autorelease];
    c1 = [[[NSTableColumn alloc] initWithIdentifier:@"alias"] autorelease];
    [[c1 headerCell] setStringValue:@"Name"];
    [c1 setWidth:200];
    c2 = [[[NSTableColumn alloc] initWithIdentifier:@"model"] autorelease];
    [[c2 headerCell] setStringValue:@"Device"];
    [c2 setWidth:110];
    c3 = [[[NSTableColumn alloc] initWithIdentifier:@"addr"] autorelease];
    [[c3 headerCell] setStringValue:@"Address"];
    [c3 setWidth:150];
    [table addTableColumn:c1];
    [table addTableColumn:c2];
    [table addTableColumn:c3];
    [table setDataSource:self];
    [table setDelegate:self];
    [scroll setDocumentView:table];
    [content addSubview:scroll];

    sendButton = [self buttonAt:NSMakeRect(20, 112, 130, 28) title:@"Send File..." action:@selector(sendFile:)];
    [sendButton setEnabled:NO];
    [content addSubview:sendButton];
    refresh = [self buttonAt:NSMakeRect(160, 112, 100, 28) title:@"Refresh" action:@selector(refresh:)];
    [content addSubview:refresh];

    bar = [[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 84, 480, 16)] autorelease];
    [bar setStyle:NSProgressIndicatorBarStyle];
    [bar setIndeterminate:NO];
    [bar setMinValue:0];
    [bar setMaxValue:100];
    [bar setAutoresizingMask:NSViewWidthSizable];
    [content addSubview:bar];

    status = [self labelAt:NSMakeRect(20, 58, 480, 17) text:@"Starting..."];
    [status setAutoresizingMask:NSViewWidthSizable];
    [content addSubview:status];

    [content addSubview:[self labelAt:NSMakeRect(20, 24, 70, 17) text:@"Save to:"]];
    folderLabel = [self labelAt:NSMakeRect(80, 24, 310, 17) text:downloadDir];
    [folderLabel setAutoresizingMask:NSViewWidthSizable];
    [content addSubview:folderLabel];
    folderButton = [self buttonAt:NSMakeRect(400, 18, 100, 28) title:@"Change..." action:@selector(chooseFolder:)];
    [folderButton setAutoresizingMask:NSViewMinXMargin];
    [content addSubview:folderButton];

    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    memset(&cb, 0, sizeof cb);
    cb.peer = cb_peer;
    cb.incoming = cb_incoming;
    cb.progress = cb_progress;
    cb.received = cb_received;
    cb.log = cb_log;
    core = ls_start([alias UTF8String], [downloadDir fileSystemRepresentation],
                    [[self dataDir] fileSystemRepresentation], LS_PORT, &cb);
    if (!core)
        [status setStringValue:@"Could not start: is another LocalSend using port 53317?"];
    else if (![self encryptionEnabled])
        ls_set_encryption(core, 0);

    [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                   selector:@selector(reload:) userInfo:nil repeats:YES];
    [self performSelector:@selector(autoCheck) withObject:nil afterDelay:6.0];
}

- (void)applicationWillTerminate:(NSNotification *)n
{
    if (core) ls_stop(core);
    core = NULL;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }

/* ---- device name ------------------------------------------------------- */

- (void)nameChanged:(id)sender
{
    NSString *name = [[nameField stringValue] stringByTrimmingCharactersInSet:
                      [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if ([name length] == 0) name = defaultName;
    if ([name length] > 40) name = [name substringToIndex:40];
    [nameField setStringValue:name];
    [[NSUserDefaults standardUserDefaults] setObject:name forKey:@"deviceName"];
    if (core) ls_set_alias(core, [name UTF8String]);
    [status setStringValue:[NSString stringWithFormat:@"This device is now called \"%@\".", name]];
}

- (void)controlTextDidEndEditing:(NSNotification *)n
{
    if ([n object] == nameField) [self nameChanged:nameField];
}

/* ---- table ------------------------------------------------------------- */

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { return npeers; }

- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
    NSString *ident = [col identifier];
    ls_peer *p;
    if (row < 0 || row >= npeers) return @"";
    p = &peers[row];
    if ([ident isEqualToString:@"alias"]) {
        NSString *a = [NSString stringWithUTF8String:p->alias];
        if (strcmp(p->protocol, "http") != 0 && !ls_https_available())
            a = [a stringByAppendingString:@"  (needs HTTPS)"];
        return a ? a : @"?";
    }
    if ([ident isEqualToString:@"model"]) return [NSString stringWithUTF8String:p->deviceModel];
    return [NSString stringWithFormat:@"%s:%d", p->ip, p->port];
}

- (void)tableViewSelectionDidChange:(NSNotification *)n { [self updateButtons]; }

- (BOOL)selectedPeerIsSendable
{
    NSInteger r = [table selectedRow];
    return core && r >= 0 && r < npeers &&
           (strcmp(peers[r].protocol, "http") == 0 || ls_https_available());
}

- (void)updateButtons { [sendButton setEnabled:[self selectedPeerIsSendable]]; }

- (void)reload:(id)sender
{
    NSString *selFp = nil;
    NSInteger r = [table selectedRow], i;
    if (!core) return;
    if (r >= 0 && r < npeers) selFp = [NSString stringWithUTF8String:peers[r].fingerprint];
    npeers = ls_peer_list(core, peers, LS_MAX_PEERS);
    [table reloadData];
    if (selFp) {
        for (i = 0; i < npeers; i++)
            if ([selFp isEqualToString:[NSString stringWithUTF8String:peers[i].fingerprint]])
                [table selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
    }
    [self updateButtons];
}

- (void)refresh:(id)sender
{
    if (core) ls_announce(core);
    [status setStringValue:@"Looking for devices..."];
}

/* ---- actions ----------------------------------------------------------- */

- (void)chooseFolder:(id)sender
{
    NSOpenPanel *op = [NSOpenPanel openPanel];
    [op setCanChooseFiles:NO];
    [op setCanChooseDirectories:YES];
    [op setPrompt:@"Choose"];
    if ([op runModalForDirectory:downloadDir file:nil types:nil] == NSOKButton) {
        NSString *dir = [[op filenames] objectAtIndex:0];
        [downloadDir release];
        downloadDir = [dir retain];
        [folderLabel setStringValue:downloadDir];
        if (core) ls_set_download_dir(core, [downloadDir fileSystemRepresentation]);
        [status setStringValue:@"Received files will be saved to the new folder."];
        [[NSUserDefaults standardUserDefaults] setObject:downloadDir forKey:@"downloadDir"];
    }
}

- (void)sendFile:(id)sender
{
    NSOpenPanel *op = [NSOpenPanel openPanel];
    NSInteger r = [table selectedRow];
    if (![self selectedPeerIsSendable]) return;
    [op setAllowsMultipleSelection:YES];
    [op setCanChooseDirectories:NO];
    if ([op runModalForDirectory:nil file:nil types:nil] != NSOKButton) return;
    {
        NSDictionary *job = [NSDictionary dictionaryWithObjectsAndKeys:
                             [NSData dataWithBytes:&peers[r] length:sizeof(ls_peer)], @"peer",
                             [op filenames], @"files", nil];
        [sendButton setEnabled:NO];
        [NSThread detachNewThreadSelector:@selector(sendJob:) toTarget:self withObject:job];
    }
}

- (void)sendJob:(NSDictionary *)job
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    ls_peer p;
    NSArray *files = [job objectForKey:@"files"];
    const char **paths;
    NSUInteger i, n = [files count];
    int rc;
    [[job objectForKey:@"peer"] getBytes:&p length:sizeof p];
    paths = (const char **)calloc(n, sizeof(char *));
    for (i = 0; i < n; i++) paths[i] = [[files objectAtIndex:i] fileSystemRepresentation];
    rc = ls_send(core, &p, (int)n, paths);
    free(paths);
    [self performSelectorOnMainThread:@selector(sendFinished:)
                           withObject:[NSNumber numberWithInt:rc] waitUntilDone:NO];
    [pool release];
}

- (void)sendFinished:(NSNumber *)rc
{
    [bar setDoubleValue:0];
    [self updateButtons];
    if ([rc intValue] == 0) [status setStringValue:@"Sent."];
    else if ([rc intValue] == -3) [status setStringValue:@"The other device declined."];
    else if ([rc intValue] == -2) [status setStringValue:[NSString stringWithFormat:
        @"Cannot send to an HTTPS device: %s", ls_https_error()]];
    else [status setStringValue:@"Sending failed. Is the other device reachable and accepting?"];
}

/* ---- main-thread helpers for callbacks --------------------------------- */

- (void)setStatusText:(NSString *)s { [status setStringValue:s]; }

- (void)setProgressInfo:(NSDictionary *)d
{
    [bar setDoubleValue:[[d objectForKey:@"pct"] doubleValue]];
    [status setStringValue:[d objectForKey:@"text"]];
}

- (void)askIncoming:(NSMutableDictionary *)req
{
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    [a setMessageText:@"Incoming files"];
    [a setInformativeText:[req objectForKey:@"message"]];
    [a addButtonWithTitle:@"Accept"];
    [a addButtonWithTitle:@"Decline"];
    [NSApp activateIgnoringOtherApps:YES];
    [req setObject:[NSNumber numberWithBool:([a runModal] == NSAlertFirstButtonReturn)]
            forKey:@"accept"];
}

/* ---- About window ------------------------------------------------------ */

- (void)openRepo:(id)sender
{
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@LS_REPO_URL]];
}

- (void)openProject:(id)sender
{
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@LS_PROJECT_URL]];
}

- (void)aboutCheck:(id)sender
{
    [self checkForUpdates:sender];
}

- (void)showAbout:(id)sender
{
    NSView *c;
    NSImageView *icon;
    NSTextField *t;
    NSString *tls;

    if (!aboutWindow) {
        aboutWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 330)
                                                  styleMask:(NSTitledWindowMask | NSClosableWindowMask)
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        [aboutWindow setTitle:@"About LocalSend for PowerPC"];
        [aboutWindow setReleasedWhenClosed:NO];
        c = [aboutWindow contentView];

        icon = [[[NSImageView alloc] initWithFrame:NSMakeRect(20, 230, 80, 80)] autorelease];
        [icon setImage:[NSApp applicationIconImage]];
        [c addSubview:icon];

        t = [self labelAt:NSMakeRect(112, 284, 270, 22) text:@"LocalSend for PowerPC"];
        [t setFont:[NSFont boldSystemFontOfSize:16]];
        [c addSubview:t];
        [c addSubview:[self labelAt:NSMakeRect(112, 262, 270, 17)
                               text:[NSString stringWithFormat:@"Version %@", [self appVersion]]]];
        tls = ls_https_available() ? @"HTTPS: available" : @"HTTPS: not available in this build";
        [c addSubview:[self labelAt:NSMakeRect(112, 244, 270, 17) text:tls]];

        t = [self labelAt:NSMakeRect(20, 150, 360, 70)
                     text:@"A LocalSend-compatible app for older Macs. It finds other LocalSend "
                          @"devices on your network and sends and receives files with them."];
        [t setFont:[NSFont systemFontOfSize:11]];
        [c addSubview:t];

        [c addSubview:[self labelAt:NSMakeRect(20, 128, 360, 17) text:@"Source code and releases:"]];
        t = [self labelAt:NSMakeRect(20, 110, 360, 17) text:@LS_REPO_URL];
        [t setTextColor:[NSColor blueColor]];
        [c addSubview:t];
        [c addSubview:[self buttonAt:NSMakeRect(20, 70, 170, 28) title:@"Open GitHub Repository"
                              action:@selector(openRepo:)]];
        [c addSubview:[self buttonAt:NSMakeRect(200, 70, 180, 28) title:@"Retrofitting Project Page"
                              action:@selector(openProject:)]];
        [c addSubview:[self buttonAt:NSMakeRect(20, 20, 170, 28) title:@"Check for Updates..."
                              action:@selector(aboutCheck:)]];
        t = [self labelAt:NSMakeRect(200, 26, 180, 17) text:@"Not affiliated with LocalSend."];
        [t setFont:[NSFont systemFontOfSize:10]];
        [c addSubview:t];
        [aboutWindow center];
    }
    [aboutWindow makeKeyAndOrderFront:nil];
}

/* ---- updates ----------------------------------------------------------- */

- (BOOL)autoUpdateEnabled
{
    id v = [[NSUserDefaults standardUserDefaults] objectForKey:@"autoUpdate"];
    return v ? [v boolValue] : YES;
}

- (void)toggleAutoUpdate:(id)sender
{
    [[NSUserDefaults standardUserDefaults] setBool:![self autoUpdateEnabled] forKey:@"autoUpdate"];
}

- (BOOL)encryptionEnabled
{
    id v = [[NSUserDefaults standardUserDefaults] objectForKey:@"encryption"];
    return v ? [v boolValue] : YES;
}

- (void)toggleEncryption:(id)sender
{
    BOOL on = ![self encryptionEnabled];
    [[NSUserDefaults standardUserDefaults] setBool:on forKey:@"encryption"];
    if (core) ls_set_encryption(core, on ? 1 : 0);
    [status setStringValue:on ? @"Encryption is on."
        : @"Encryption is off: transfers are faster, but files cross your network unencrypted."];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if ([item action] == @selector(toggleEncryption:)) {
        [item setState:[self encryptionEnabled] ? NSOnState : NSOffState];
        return ls_https_available();
    }
    if ([item action] == @selector(toggleAutoUpdate:))
        [item setState:[self autoUpdateEnabled] ? NSOnState : NSOffState];
    if ([item action] == @selector(checkForUpdates:)) return !checking && !installing;
    return YES;
}

- (void)autoCheck
{
    NSDate *last = [[NSUserDefaults standardUserDefaults] objectForKey:@"lastUpdateCheck"];
    if (![self autoUpdateEnabled]) return;
    if (last && [[NSDate date] timeIntervalSinceDate:last] < 24 * 3600) return;
    [self startUpdateCheck:NO];
}

- (void)checkForUpdates:(id)sender { [self startUpdateCheck:YES]; }

- (void)startUpdateCheck:(BOOL)user
{
    if (checking || installing) return;
    checking = YES;
    if (user) [status setStringValue:@"Checking for updates..."];
    [NSThread detachNewThreadSelector:@selector(updateCheckThread:) toTarget:self
                           withObject:[NSNumber numberWithBool:user]];
}

- (void)updateCheckThread:(NSNumber *)user
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    ls_release rel;
    char err[300];
    NSString *ca = [self caPath];
    NSMutableDictionary *d = [NSMutableDictionary dictionaryWithObject:user forKey:@"user"];
    err[0] = 0;
    if (!ca) {
        [d setObject:@"The certificate list is missing from the app." forKey:@"error"];
    } else if (ls_update_check(LS_UPDATE_API, LS_REPO, LS_UPDATE_ASSET, [ca fileSystemRepresentation],
                               &rel, err, sizeof err) != 0) {
        [d setObject:[NSString stringWithUTF8String:err] forKey:@"error"];
    } else {
        [d setObject:[NSData dataWithBytes:&rel length:sizeof rel] forKey:@"release"];
    }
    [self performSelectorOnMainThread:@selector(updateCheckDone:) withObject:d waitUntilDone:NO];
    [pool release];
}

- (void)updateCheckDone:(NSDictionary *)d
{
    BOOL user = [[d objectForKey:@"user"] boolValue];
    NSData *relData = [d objectForKey:@"release"];
    NSString *err = [d objectForKey:@"error"];
    ls_release rel;
    NSString *version, *current = [self appVersion];
    NSAlert *a;
    NSInteger hit;

    checking = NO;
    [status setStringValue:@"Ready."];
    if (err) {
        if (user) {
            a = [[[NSAlert alloc] init] autorelease];
            [a setMessageText:@"Could not check for updates"];
            [a setInformativeText:err];
            [a runModal];
        } else {
            [status setStringValue:[NSString stringWithFormat:@"Update check failed: %@", err]];
        }
        return;
    }
    [[NSUserDefaults standardUserDefaults] setObject:[NSDate date] forKey:@"lastUpdateCheck"];
    [relData getBytes:&rel length:sizeof rel];
    version = [NSString stringWithUTF8String:rel.version];

    if (ls_version_cmp(rel.version, [current UTF8String]) <= 0) {
        if (user) {
            a = [[[NSAlert alloc] init] autorelease];
            [a setMessageText:@"You're up to date"];
            [a setInformativeText:[NSString stringWithFormat:@"LocalSend for PowerPC %@ is the newest version.", current]];
            [a runModal];
        }
        return;
    }
    if (!user && [version isEqualToString:[[NSUserDefaults standardUserDefaults] stringForKey:@"skipVersion"]])
        return;

    a = [[[NSAlert alloc] init] autorelease];
    [a setMessageText:[NSString stringWithFormat:@"Version %@ is available", version]];
    [a setInformativeText:[NSString stringWithFormat:@"You have version %@.\n\n%.600s", current, rel.notes]];
    [a addButtonWithTitle:@"Install and Restart"];
    [a addButtonWithTitle:@"Later"];
    [a addButtonWithTitle:@"Skip This Version"];
    [NSApp activateIgnoringOtherApps:YES];
    hit = [a runModal];
    if (hit == NSAlertFirstButtonReturn) {
        [self beginInstall:relData];
    } else if (hit == NSAlertFirstButtonReturn + 2) {
        [[NSUserDefaults standardUserDefaults] setObject:version forKey:@"skipVersion"];
    }
}

- (NSString *)cacheDir
{
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Caches/LocalSendPPC"];
}

- (void)beginInstall:(NSData *)relData
{
    NSString *bundle = [[NSBundle mainBundle] bundlePath];
    NSString *parent = [bundle stringByDeletingLastPathComponent];
    NSAlert *a;
    if (![[NSFileManager defaultManager] isWritableFileAtPath:parent]) {
        a = [[[NSAlert alloc] init] autorelease];
        [a setMessageText:@"Cannot update here"];
        [a setInformativeText:[NSString stringWithFormat:
            @"LocalSend cannot replace itself in \"%@\". Move the app to a folder you own (for example your Applications folder) and try again.", parent]];
        [a runModal];
        return;
    }
    installing = YES;
    [bar setDoubleValue:0];
    [status setStringValue:@"Downloading update..."];
    [NSThread detachNewThreadSelector:@selector(installThread:) toTarget:self withObject:relData];
}

- (void)installThread:(NSData *)relData
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    ls_release rel;
    char err[300];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *cache = [self cacheDir];
    NSString *zip = [cache stringByAppendingPathComponent:@"update.zip"];
    NSString *stage = [cache stringByAppendingPathComponent:@"stage"];
    NSString *failure = nil, *newApp = nil;
    NSTask *task;

    [relData getBytes:&rel length:sizeof rel];
    [fm removeFileAtPath:stage handler:nil];
    [fm createDirectoryAtPath:stage attributes:nil];
    [fm removeFileAtPath:zip handler:nil];
    err[0] = 0;
    /* download, signature check */
    if (ls_update_download(&rel, [[self caPath] fileSystemRepresentation], [zip fileSystemRepresentation],
                           dl_progress, NULL, err, sizeof err) != 0) {
        failure = [NSString stringWithUTF8String:err];
    } else {
        /* unpack and make sure it really is a newer LocalSend */
        task = [[[NSTask alloc] init] autorelease];
        [task setLaunchPath:@"/usr/bin/unzip"];
        [task setArguments:[NSArray arrayWithObjects:@"-q", @"-o", zip, @"-d", stage, nil]];
        [task launch];
        [task waitUntilExit];
        newApp = [stage stringByAppendingPathComponent:@"LocalSend.app"];
        if ([task terminationStatus] != 0 ||
            ![fm fileExistsAtPath:[newApp stringByAppendingPathComponent:@"Contents/MacOS/LocalSend"]]) {
            failure = @"The downloaded update could not be unpacked.";
        } else {
            NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:
                                  [newApp stringByAppendingPathComponent:@"Contents/Info.plist"]];
            NSString *nv = [info objectForKey:@"CFBundleVersion"];
            if (!nv || ls_version_cmp([nv UTF8String], [[self appVersion] UTF8String]) <= 0)
                failure = @"The downloaded version is not newer than this one, update cancelled.";
        }
    }
    [self performSelectorOnMainThread:@selector(installReady:)
                           withObject:(failure ? [NSDictionary dictionaryWithObject:failure forKey:@"error"]
                                               : [NSDictionary dictionaryWithObject:newApp forKey:@"newApp"])
                        waitUntilDone:NO];
    [pool release];
}

- (void)installReady:(NSDictionary *)d
{
    NSString *err = [d objectForKey:@"error"];
    NSString *newApp = [d objectForKey:@"newApp"];
    NSString *script, *scriptPath;
    NSTask *task;
    NSAlert *a;

    installing = NO;
    [bar setDoubleValue:0];
    if (err) {
        [status setStringValue:@"Update failed."];
        a = [[[NSAlert alloc] init] autorelease];
        [a setMessageText:@"The update was not installed"];
        [a setInformativeText:err];
        [a runModal];
        return;
    }
    /* a small script swaps the app once this process has quit, then reopens it */
    script = @"#!/bin/sh\n"
             @"PID=$1; NEW=\"$2\"; OLD=\"$3\"\n"
             @"n=0; while kill -0 $PID 2>/dev/null && [ $n -lt 60 ]; do sleep 1; n=$((n+1)); done\n"
             @"if mv \"$OLD\" \"$OLD.old\"; then\n"
             @"  if mv \"$NEW\" \"$OLD\"; then rm -rf \"$OLD.old\"; else mv \"$OLD.old\" \"$OLD\"; fi\n"
             @"fi\n"
             @"open \"$OLD\"\n";
    scriptPath = [[self cacheDir] stringByAppendingPathComponent:@"install.sh"];
    if (![script writeToFile:scriptPath atomically:YES]) {
        [status setStringValue:@"Update failed."];
        return;
    }
    [status setStringValue:@"Installing update, LocalSend will restart..."];
    task = [[[NSTask alloc] init] autorelease];
    [task setLaunchPath:@"/bin/sh"];
    [task setArguments:[NSArray arrayWithObjects:@"-c",
        @"nohup /bin/sh \"$0\" \"$1\" \"$2\" \"$3\" >/dev/null 2>&1 &",
        scriptPath, [NSString stringWithFormat:@"%d", (int)getpid()],
        newApp, [[NSBundle mainBundle] bundlePath], nil]];
    [task launch];
    [NSApp terminate:nil];
}

@end
