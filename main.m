/*
 * rome: a small, fast terminal for GNUstep. Application, windows and menus.
 *
 * Defaults (also as command-line arguments, e.g. -RomeRenderer gl):
 *   RomeRenderer      x11 (CPU, damage-only; the default) or gl (OpenGL)
 *   RomeFont          fontconfig family (DejaVu Sans Mono)
 *   RomeFontSize      pixels (13)
 *   RomeColumns, RomeRows   initial size (80 x 24)
 *   RomeScrollback    lines (5000)
 *   RomeMaxFPS        frame cap (60)
 *   RomeCursorBlink   YES
 *   RomeOptionAsMeta  YES (Option/Alt sends ESC-prefixed keys)
 *   RomeTheme         Basic (black on white) or Pro (light on black)
 *   RomeCommand       run "/bin/sh -c <command>" instead of the login shell
 *   RomeQuitOnExit    quit when the command exits (benchmarks)
 *   RomeBenchType     type N characters into the pty, 20 a second, then ^D
 * Environment: ROME_STATS=1 prints frame costs (and at exit).
 */
#import <AppKit/AppKit.h>
#import "RomeView.h"
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>

@interface RomeController : NSObject
{
	int windows;
	NSPoint cascade;
	RomeView *benchView;
	int benchLeft;
}
@end

static id
add_item(NSMenu *m, NSString *title, SEL action, NSString *key)
{
	return [m addItemWithTitle: title action: action keyEquivalent: key ? key : @""];
}

@implementation RomeController

- (void) buildMenus
{
	NSMenu *main = [[NSMenu alloc] initWithTitle: @"Rome"];
	NSMenu *m;

	m = [[NSMenu alloc] initWithTitle: @"Rome"];
	add_item(m, @"About Rome", @selector(orderFrontStandardAboutPanel:), nil);
	add_item(m, @"Hide Rome", @selector(hide:), @"h");
	add_item(m, @"Quit Rome", @selector(terminate:), @"q");
	[main setSubmenu: m forItem: add_item(main, @"Rome", NULL, nil)];
	[m release];

	m = [[NSMenu alloc] initWithTitle: @"Shell"];
	add_item(m, @"New Window", @selector(newWindow:), @"n");
	add_item(m, @"Close Window", @selector(performClose:), @"w");
	[main setSubmenu: m forItem: add_item(main, @"Shell", NULL, nil)];
	[m release];

	m = [[NSMenu alloc] initWithTitle: @"Edit"];
	add_item(m, @"Copy", @selector(copy:), @"c");
	add_item(m, @"Paste", @selector(paste:), @"v");
	add_item(m, @"Select All", @selector(selectAll:), @"a");
	add_item(m, @"Clear Scrollback", @selector(clearScrollback:), @"k");
	[main setSubmenu: m forItem: add_item(main, @"Edit", NULL, nil)];
	[m release];

	m = [[NSMenu alloc] initWithTitle: @"Window"];
	add_item(m, @"Minimize", @selector(performMiniaturize:), @"m");
	[main setSubmenu: m forItem: add_item(main, @"Window", NULL, nil)];
	[NSApp setWindowsMenu: m];
	[m release];

	[NSApp setMainMenu: main];
	[main release];
}

- (NSArray *) command
{
	NSString *cmd = [[NSUserDefaults standardUserDefaults] stringForKey: @"RomeCommand"];
	if (cmd == nil)
		return nil;
	return [NSArray arrayWithObjects: @"/bin/sh", @"-c", cmd, nil];
}

- (RomeView *) openWindow
{
	NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
	int cols = [ud objectForKey: @"RomeColumns"] ? (int)[ud integerForKey: @"RomeColumns"] : 80;
	int rows = [ud objectForKey: @"RomeRows"] ? (int)[ud integerForKey: @"RomeRows"] : 24;
	NSSize size = [RomeView contentSizeForColumns: cols rows: rows];
	NSRect frame = NSMakeRect(80, 80, size.width, size.height);
	NSWindow *w = [[NSWindow alloc] initWithContentRect: frame
	    styleMask: NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask | NSResizableWindowMask
	    backing: NSBackingStoreBuffered defer: NO];
	[w setTitle: @"Terminal"];
	[w setReleasedWhenClosed: YES];
	RomeView *v = [[RomeView alloc] initWithFrame: NSMakeRect(0, 0, size.width, size.height) command: [self command]];
	if (v == nil) {
		[w release];
		return nil;
	}
	[w setContentView: v];
	[w setResizeIncrements: [v cellSize]];
	[w setMinSize: NSMakeSize(200, 100)];
	if (windows > 0)
		cascade = [w cascadeTopLeftFromPoint: cascade];
	else
		cascade = [w cascadeTopLeftFromPoint: NSMakePoint(80, NSMaxY([[w screen] visibleFrame]) - 40)];
	[w makeKeyAndOrderFront: nil];
	[w makeFirstResponder: v];
	/* the view must be in a mapped window before its X child is made */
	[v performSelector: @selector(start)];
	[v release];
	windows++;
	[[NSNotificationCenter defaultCenter] addObserver: self selector: @selector(windowClosed:)
	    name: NSWindowWillCloseNotification object: w];
	return v;
}

- (void) newWindow: (id)sender
{
	[self openWindow];
}

- (void) windowClosed: (NSNotification *)n
{
	windows--;
	if (windows <= 0)
		[NSApp terminate: self];
}

- (void) childExited: (NSNotification *)n
{
	RomeView *v = [n object];
	int status = [[[n userInfo] objectForKey: @"status"] intValue];
	if ([[NSUserDefaults standardUserDefaults] boolForKey: @"RomeQuitOnExit"]) {
		[v printStats];
		fflush(stderr);
		exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
	}
	/* Terminal's default: close the window when the shell exited cleanly */
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		[[v window] performSelector: @selector(performClose:) withObject: nil afterDelay: 0];
}

- (void) benchType: (NSTimer *)t
{
	if (benchLeft > 0) {
		[benchView sendBenchKey: benchLeft % 40 == 1 ? '\n' : 'a' + benchLeft % 26];
		benchLeft--;
		return;
	}
	[t invalidate];
	[benchView sendBenchKey: 4];    /* ^D: the command (cat) ends */
}

- (void) applicationDidFinishLaunching: (NSNotification *)n
{
	[self buildMenus];
	[[NSNotificationCenter defaultCenter] addObserver: self selector: @selector(childExited:)
	    name: RomeViewChildExitedNotification object: nil];
	RomeView *v = [self openWindow];
	if (v == nil) {
		NSLog(@"rome: cannot open a terminal window");
		exit(1);
	}
	int nb = (int)[[NSUserDefaults standardUserDefaults] integerForKey: @"RomeBenchType"];
	if (nb > 0) {
		benchView = v;
		benchLeft = nb;
		NSTimer *t = [NSTimer timerWithTimeInterval: 0.05 target: self selector: @selector(benchType:)
		    userInfo: nil repeats: YES];
		[t setFireDate: [NSDate dateWithTimeIntervalSinceNow: 2.0]];
		[[NSRunLoop currentRunLoop] addTimer: t forMode: NSDefaultRunLoopMode];
	}
}

- (BOOL) applicationShouldTerminateAfterLastWindowClosed: (NSApplication *)app
{
	return YES;
}

@end

int
main(int argc, const char **argv)
{
	NSAutoreleasePool *pool = [NSAutoreleasePool new];
	signal(SIGPIPE, SIG_IGN);
	[NSApplication sharedApplication];
	[NSApp setDelegate: [RomeController new]];
	[NSApp run];
	[pool release];
	return 0;
}
