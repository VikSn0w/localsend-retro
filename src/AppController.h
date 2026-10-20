#import <Cocoa/Cocoa.h>

/* Mac OS X 10.3/10.4 SDKs have no NSInteger */
#ifndef NSINTEGER_DEFINED
typedef int NSInteger;
typedef unsigned int NSUInteger;
#define NSINTEGER_DEFINED 1
#endif
#ifndef NSAlertFirstButtonReturn
#define NSAlertFirstButtonReturn 1000
#endif
#include "ls.h"

@interface AppController : NSObject {
    ls_ctx *core;
    NSWindow *window;
    NSWindow *aboutWindow;
    NSTableView *table;
    NSTextField *status;
    NSTextField *folderLabel;
    NSTextField *nameField;
    NSProgressIndicator *bar;
    NSButton *sendButton;
    NSButton *encCheck;
    NSString *downloadDir;
    NSString *defaultName;
    BOOL checking;
    BOOL installing;
    ls_peer peers[LS_MAX_PEERS];
    int npeers;
}
- (void)showAbout:(id)sender;
- (void)checkForUpdates:(id)sender;
- (void)toggleAutoUpdate:(id)sender;
- (void)toggleEncryption:(id)sender;
- (void)syncEncryptionControls;
@end
