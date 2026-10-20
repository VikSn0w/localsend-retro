#import <Cocoa/Cocoa.h>
#import "AppController.h"

static NSMenuItem *add_item(NSMenu *menu, NSString *title, SEL action, NSString *key, id target)
{
    NSMenuItem *it = [menu addItemWithTitle:title action:action keyEquivalent:key];
    if (target) [it setTarget:target];
    return it;
}

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    AppController *ctl;
    NSMenu *menubar, *appMenu, *editMenu;
    NSMenuItem *appItem, *editItem;

    [NSApplication sharedApplication];
    ctl = [[AppController alloc] init];

    menubar = [[[NSMenu alloc] init] autorelease];

    appItem = [[[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""] autorelease];
    [menubar addItem:appItem];
    appMenu = [[[NSMenu alloc] initWithTitle:@"LocalSend"] autorelease];
    add_item(appMenu, @"About LocalSend", @selector(showAbout:), @"", ctl);
    [appMenu addItem:[NSMenuItem separatorItem]];
    add_item(appMenu, @"Check for Updates...", @selector(checkForUpdates:), @"", ctl);
    add_item(appMenu, @"Check for Updates Automatically", @selector(toggleAutoUpdate:), @"", ctl);
    [appMenu addItem:[NSMenuItem separatorItem]];
    add_item(appMenu, @"Quit LocalSend", @selector(terminate:), @"q", nil);
    [appItem setSubmenu:appMenu];

    /* needed so Copy and Paste work in the device name field */
    editItem = [[[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""] autorelease];
    [menubar addItem:editItem];
    editMenu = [[[NSMenu alloc] initWithTitle:@"Edit"] autorelease];
    add_item(editMenu, @"Undo", @selector(undo:), @"z", nil);
    [editMenu addItem:[NSMenuItem separatorItem]];
    add_item(editMenu, @"Cut", @selector(cut:), @"x", nil);
    add_item(editMenu, @"Copy", @selector(copy:), @"c", nil);
    add_item(editMenu, @"Paste", @selector(paste:), @"v", nil);
    add_item(editMenu, @"Select All", @selector(selectAll:), @"a", nil);
    [editItem setSubmenu:editMenu];

    [NSApp setMainMenu:menubar];
    [NSApp setDelegate:ctl];
    [NSApp run];

    [pool release];
    return 0;
}
