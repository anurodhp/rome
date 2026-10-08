/*
 * rome: the terminal view (see RomeView.h).
 */
#import "RomeView.h"
#import <GNUstepGUI/GSDisplayServer.h>
#include "RomeX.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>

NSString *RomeViewChildExitedNotification = @"RomeViewChildExitedNotification";

/* Mac Terminal's "Basic" profile: black on white, its ANSI colours. */
static const RomeTheme theme_basic = {
	.fg = 0x000000, .bg = 0xffffff, .cursor = 0x7f7f7f, .selection = 0xb4d5fe,
	.palette = { 0x000000, 0x990000, 0x00a600, 0x999900, 0x0000b2, 0xb200b2, 0x00a6b2, 0xbfbfbf,
	             0x666666, 0xe50000, 0x00d900, 0xe5e500, 0x0000ff, 0xe500e5, 0x00e5e5, 0xe5e5e5 },
};
/* "Pro": light grey on black */
static const RomeTheme theme_pro = {
	.fg = 0xf2f2f2, .bg = 0x000000, .cursor = 0x4d4d4d, .selection = 0x414f78,
	.palette = { 0x000000, 0x990000, 0x00a600, 0x999900, 0x2f39d1, 0xb200b2, 0x00a6b2, 0xbfbfbf,
	             0x666666, 0xe50000, 0x00d900, 0xe5e500, 0x0000ff, 0xe500e5, 0x00e5e5, 0xe5e5e5 },
};

/* the default: white on black, Pro's ANSI colours */
static const RomeTheme theme_dark = {
	.fg = 0xffffff, .bg = 0x000000, .cursor = 0x4d4d4d, .selection = 0x414f78,
	.palette = { 0x000000, 0xcc3333, 0x00a600, 0x999900, 0x4a6bff, 0xb200b2, 0x00a6b2, 0xbfbfbf,
	             0x666666, 0xe50000, 0x00d900, 0xe5e500, 0x6b8cff, 0xe500e5, 0x00e5e5, 0xffffff },
};

static BOOL g_stats;

static const RomeTheme *
theme_named(NSString *name)
{
	if ([name caseInsensitiveCompare: @"Basic"] == NSOrderedSame)
		return &theme_basic;
	if ([name caseInsensitiveCompare: @"Pro"] == NSOrderedSame)
		return &theme_pro;
	return &theme_dark;
}

static NSString *
default_string(NSString *key, NSString *def)
{
	NSString *s = [[NSUserDefaults standardUserDefaults] stringForKey: key];
	return s ? s : def;
}
static double
default_double(NSString *key, double def)
{
	id o = [[NSUserDefaults standardUserDefaults] objectForKey: key];
	return o ? [o doubleValue] : def;
}
static BOOL
default_bool(NSString *key, BOOL def)
{
	id o = [[NSUserDefaults standardUserDefaults] objectForKey: key];
	return o ? [o boolValue] : def;
}

static RomeFont *
open_font(void)
{
	NSString *fam = default_string(@"RomeFont", @"DejaVu Sans Mono");
	double px = default_double(@"RomeFontSize", 13);
	RomeFont *f = rome_font_new([fam UTF8String], px);
	if (f == NULL)
		f = rome_font_new("monospace", px);
	return f;
}

/* ---- run loop glue: the pty fd and rome's X connection ---- */

@interface RomeXWatcher : NSObject <RunLoopEvents>
+ (void) watch;
@end

@implementation RomeXWatcher
+ (void) watch
{
	static RomeXWatcher *w;
	Display *dpy = rome_x_display();
	if (w != nil || dpy == NULL)
		return;
	w = [RomeXWatcher new];
	NSRunLoop *rl = [NSRunLoop currentRunLoop];
	void *fd = (void *)(intptr_t)ConnectionNumber(dpy);
	[rl addEvent: fd type: ET_RDESC watcher: w forMode: NSDefaultRunLoopMode];
	[rl addEvent: fd type: ET_RDESC watcher: w forMode: NSEventTrackingRunLoopMode];
	[rl addEvent: fd type: ET_RDESC watcher: w forMode: NSModalPanelRunLoopMode];
}
- (void) receivedEvent: (void *)data type: (RunLoopEventType)type extra: (void *)extra forMode: (NSString *)mode
{
	rome_x_pump();
}
@end

static void
expose_cb(void *owner, int x, int y, int w, int h)
{
	RomeView *v = owner;
	[v performSelector: @selector(exposeX:) withObject: [NSValue valueWithRect: NSMakeRect(x, y, w, h)]];
}

static void
title_cb(void *owner, const char *utf8)
{
	[(RomeView *)owner performSelector: @selector(applicationSetTitle:)
	    withObject: [NSString stringWithUTF8String: utf8]];
}

/* The command name of process `pid`, or nil. */
static NSString *
process_name(pid_t pid)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
	struct kinfo_proc kp;
	size_t len = sizeof(kp);
	memset(&kp, 0, sizeof(kp));
	if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len == 0 || kp.kp_proc.p_comm[0] == 0)
		return nil;
	return [NSString stringWithUTF8String: kp.kp_proc.p_comm];
}

static void
bell_cb(void *owner)
{
	(void)owner;
	NSBeep();
}

static void
want_write_cb(void *owner)
{
	[(RomeView *)owner performSelector: @selector(startWriting)];
}

@implementation RomeView

+ (void) initialize
{
	g_stats = getenv("ROME_STATS") != NULL;
}

+ (NSSize) contentSizeForColumns: (int)cols rows: (int)rows
{
	static RomeFont *f;
	if (f == NULL)
		f = open_font();
	int pad = (int)default_double(@"RomePadding", 4);
	CGFloat sw = [NSScroller scrollerWidth];
	if (f == NULL)
		return NSMakeSize(cols * 8 + 2 * pad + sw, rows * 16 + 2 * pad);
	return NSMakeSize(cols * f->cell_w + 2 * pad + sw, rows * f->cell_h + 2 * pad);
}

- (id) initWithFrame: (NSRect)frame command: (NSArray *)argv
{
	self = [super initWithFrame: frame];
	if (self == nil)
		return nil;
	font = open_font();
	if (font == NULL) {
		NSLog(@"rome: no usable monospaced font");
		[self release];
		return nil;
	}
	command = [argv retain];
	pad = (int)default_double(@"RomePadding", 4);
	double fps = default_double(@"RomeMaxFPS", 60);
	minFrameMs = fps > 0 ? 1000.0 / fps : 0;
	optionAsMeta = default_bool(@"RomeOptionAsMeta", YES);
	blinkEnabled = default_bool(@"RomeCursorBlink", YES);
	immediateRender = default_bool(@"RomeImmediateRender", YES);
	rendererName = [default_string(@"RomeRenderer", @"x11") retain];
	const RomeTheme *theme = theme_named(default_string(@"RomeTheme", @"Dark"));
	themeBg = theme->bg;

	RomeTermCallbacks cb = { title_cb, bell_cb, want_write_cb };
	int cols = ((int)frame.size.width - (int)[NSScroller scrollerWidth] - 2 * pad) / font->cell_w;
	int rows = ((int)frame.size.height - 2 * pad) / font->cell_h;
	term = rome_term_new(rows, cols, (int)default_double(@"RomeScrollback", 5000), theme, &cb, self);
	if (term == NULL) {
		[self release];
		return nil;
	}
	[self setAutoresizingMask: NSViewWidthSizable | NSViewHeightSizable];
	/* The X child window covers the grid only; the scroller sits beside it
	 * in the strip it leaves, where AppKit still draws and gets the mouse. */
	scroller = [[NSScroller alloc] initWithFrame: NSMakeRect(0, 0, [NSScroller scrollerWidth], frame.size.height)];
	[scroller setArrowsPosition: NSScrollerArrowsNone];
	[scroller setTarget: self];
	[scroller setAction: @selector(scrollerHit:)];
	[scroller setEnabled: NO];
	[self addSubview: scroller];
	[scroller release];
	startMs = rome_now_ms();
	return self;
}

- (void) dealloc
{
	[self shutdown];
	[command release];
	[rendererName release];
	[oscTitle release];
	[fgName release];
	[shownTitle release];
	[super dealloc];
}

- (BOOL) isFlipped { return YES; }
- (BOOL) isOpaque { return YES; }
- (BOOL) acceptsFirstResponder { return YES; }
- (BOOL) becomeFirstResponder { return YES; }
- (NSSize) cellSize { return NSMakeSize(font->cell_w, font->cell_h); }
- (id) controller { return controller; }
- (void) setController: (id)c { controller = c; }
- (NSString *) title { return shownTitle != nil ? shownTitle : @"Terminal"; }
- (const char *) rendererName { return renderer ? renderer->name : "none"; }

- (void) drawRect: (NSRect)r
{
	/* covered by the child window; shows only while it is being created */
	[[NSColor colorWithCalibratedRed: ((themeBg >> 16) & 255) / 255.0 green: ((themeBg >> 8) & 255) / 255.0
	    blue: (themeBg & 255) / 255.0 alpha: 1] set];
	NSRectFill(r);
}

/* ---- the child window ---- */

/* The view's origin in the GNUstep window's X window: as gnustep-back's
 * XGGLContext.m (-initWithView:visualinfo:) places its GLX sub-window. */
- (NSPoint) xOrigin
{
	NSWindow *w = [self window];
	GSDisplayServer *srv = GSCurrentServer();
	NSRect rect;
	CGFloat height;
	if ([srv handlesWindowDecorations]) {
		NSView *cv = [w contentView];
		rect = [self convertRect: [self bounds] toView: cv];
		if ([cv isFlipped])
			rect.origin.y = NSHeight([cv frame]) - NSMaxY(rect);
		height = NSHeight([cv frame]);
	} else {
		rect = [self convertRect: [self bounds] toView: nil];
		height = NSHeight([w frame]);
	}
	return NSMakePoint(NSMinX(rect), height - NSMaxY(rect));
}

/* The part of the view the X child covers: all of it but the scroller strip. */
- (NSSize) gridSize
{
	NSSize s = [self bounds].size;
	s.width -= [NSScroller scrollerWidth];
	if (s.width < 1)
		s.width = 1;
	return s;
}

- (void) createRenderer
{
	NSWindow *w = [self window];
	if (renderer != NULL || w == nil || term == NULL)
		return;
	GSDisplayServer *srv = GSCurrentServer();
	Window parent = (Window)(uintptr_t)[srv windowDevice: [w windowNumber]];
	if (parent == 0)
		return;
	/* the GNUstep window must exist on the server before rome's
	 * connection makes a child of it */
	SEL xd = @selector(xDisplay);
	if ([srv respondsToSelector: xd]) {
		Display *(*getdpy)(id, SEL) = (Display *(*)(id, SEL))[srv methodForSelector: xd];
		Display *gdpy = getdpy(srv, xd);
		if (gdpy != NULL)
			XSync(gdpy, False);
	}
	NSPoint o = [self xOrigin];
	NSSize s = [self gridSize];
	BOOL wantGL = [rendererName caseInsensitiveCompare: @"gl"] == NSOrderedSame;
	if (wantGL)
		renderer = rome_render_gl_new(font, parent, (int)o.x, (int)o.y, (int)s.width, (int)s.height);
	if (renderer == NULL)
		renderer = rome_render_x11_new(font, parent, (int)o.x, (int)o.y, (int)s.width, (int)s.height);
	if (renderer == NULL && !wantGL)
		renderer = rome_render_gl_new(font, parent, (int)o.x, (int)o.y, (int)s.width, (int)s.height);
	if (renderer == NULL) {
		NSLog(@"rome: no renderer could start");
		return;
	}
	if (g_stats)
		fprintf(stderr, "rome: renderer %s, cell %dx%d, font %s %.0fpx\n", renderer->name,
		    font->cell_w, font->cell_h, font->family ? font->family : "?", font->px);
	renderer->padx = pad;
	renderer->pady = pad;
	[self updatePadColor];
	rome_x_register((Window)renderer->win, expose_cb, self);
	[RomeXWatcher watch];
	[self layoutGrid];
	rome_term_damage_all(term);
	[self scheduleRender];
}

- (void) updatePadColor
{
	renderer->padbg = themeBg;
}

- (void) layoutGrid
{
	NSSize b = [self bounds].size;
	CGFloat sw = [NSScroller scrollerWidth];
	[scroller setFrame: NSMakeRect(b.width - sw, 0, sw, b.height)];
	if (renderer == NULL)
		return;
	NSSize s = [self gridSize];
	NSPoint o = [self xOrigin];
	if (s.width == laidOut.width && s.height == laidOut.height && o.x == laidOrigin.x && o.y == laidOrigin.y)
		return;
	laidOut = s;
	laidOrigin = o;
	rome_render_move(renderer, (int)o.x, (int)o.y);
	renderer->resize(renderer, (int)s.width, (int)s.height);
	int cols = ((int)s.width - 2 * pad) / font->cell_w;
	int rows = ((int)s.height - 2 * pad) / font->cell_h;
	if (g_stats)
		fprintf(stderr, "rome: layout %dx%d px -> %dx%d cells\n", (int)s.width, (int)s.height, cols, rows);
	rome_term_resize(term, rows, cols, cols * font->cell_w, rows * font->cell_h);
	rome_term_damage_all(term);
	[self scheduleRender];
}

/* ---- scroll bar ---- */

- (void) updateScroller
{
	if (term == NULL)
		return;
	int sb = rome_term_scrollback_lines(term), off = rome_term_view_offset(term), rows = rome_term_rows(term);
	if (sb == scSb && off == scOff && rows == scRows)
		return;
	scSb = sb;
	scOff = off;
	scRows = rows;
	if (sb <= 0 || rome_term_altscreen(term)) {
		[scroller setEnabled: NO];
		[scroller setFloatValue: 1 knobProportion: 1];
		return;
	}
	[scroller setEnabled: YES];
	[scroller setFloatValue: 1.0 - (double)off / sb knobProportion: (double)rows / (sb + rows)];
}

- (void) scrollerHit: (id)sender
{
	if (term == NULL)
		return;
	int sb = rome_term_scrollback_lines(term), off = rome_term_view_offset(term), rows = rome_term_rows(term);
	switch ([scroller hitPart]) {
	case NSScrollerKnob:
	case NSScrollerKnobSlot: {
		int target = (int)((1.0 - [scroller floatValue]) * sb + 0.5);
		rome_term_scroll_view(term, target - off);
		break;
	}
	case NSScrollerDecrementPage:
		rome_term_scroll_view(term, rows - 1);
		break;
	case NSScrollerIncrementPage:
		rome_term_scroll_view(term, -(rows - 1));
		break;
	default:
		return;
	}
	scSb = -1;      /* the knob may have to snap to a whole line */
	[self updateScroller];
	[self scheduleRender];
}

- (void) viewDidMoveToWindow
{
	[super viewDidMoveToWindow];
	NSWindow *w = [self window];
	if (w == nil)
		return;
	NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
	[nc addObserver: self selector: @selector(windowFocus:) name: NSWindowDidBecomeKeyNotification object: w];
	[nc addObserver: self selector: @selector(windowFocus:) name: NSWindowDidResignKeyNotification object: w];
	[nc addObserver: self selector: @selector(windowResized:) name: NSWindowDidResizeNotification object: w];
	[nc addObserver: self selector: @selector(windowWillClose:) name: NSWindowWillCloseNotification object: w];
}

- (void) start
{
	[self createRenderer];
	if (rome_term_fd(term) >= 0 || exited)
		return;
	char **argv = NULL;
	if ([command count] > 0) {
		argv = calloc([command count] + 1, sizeof(char *));
		for (NSUInteger i = 0; i < [command count]; i++)
			argv[i] = strdup([[command objectAtIndex: i] UTF8String]);
	}
	int fd = rome_term_spawn(term, argv);
	if (argv != NULL) {
		for (NSUInteger i = 0; i < [command count]; i++)
			free(argv[i]);
		free(argv);
	}
	if (fd < 0) {
		const char *msg = "rome: cannot start the shell (forkpty failed)\r\n";
		rome_term_feed(term, msg, strlen(msg));
		[self scheduleRender];
		return;
	}
	NSRunLoop *rl = [NSRunLoop currentRunLoop];
	[rl addEvent: (void *)(intptr_t)fd type: ET_RDESC watcher: self forMode: NSDefaultRunLoopMode];
	[rl addEvent: (void *)(intptr_t)fd type: ET_RDESC watcher: self forMode: NSEventTrackingRunLoopMode];
	reading = YES;
	if (g_stats) {
		probeLast = rome_now_ms();
		probeTimer = [[NSTimer scheduledTimerWithTimeInterval: 0.02 target: self selector: @selector(probe:)
		    userInfo: nil repeats: YES] retain];
	}
	titleTimer = [[NSTimer scheduledTimerWithTimeInterval: 0.5 target: self selector: @selector(pollTitle:)
	    userInfo: nil repeats: YES] retain];
	if (blinkEnabled)
		blinkTimer = [[NSTimer scheduledTimerWithTimeInterval: 0.53 target: self selector: @selector(blink:)
		    userInfo: nil repeats: YES] retain];
}

- (void) setFrameSize: (NSSize)size
{
	[super setFrameSize: size];
	[self layoutGrid];
}

- (void) setFrameOrigin: (NSPoint)p
{
	[super setFrameOrigin: p];
	if (renderer != NULL) {
		NSPoint o = [self xOrigin];
		rome_render_move(renderer, (int)o.x, (int)o.y);
	}
}

- (void) windowFocus: (NSNotification *)n
{
	rome_term_set_focus(term, !tabHidden && [[self window] isKeyWindow]);
	[self scheduleRender];
}

/* ---- window title ----
 * What the application asked for (OSC 0/2) wins; until then, and once the
 * foreground job that set it has gone, the name of the foreground process
 * on the pty (bash, vim, htop). */

- (void) showTitle: (NSString *)t
{
	if (t == nil || [t isEqualToString: shownTitle] || [self window] == nil)
		return;
	[shownTitle release];
	shownTitle = [t retain];
	if (controller != nil)
		[controller romeView: self titleChanged: t];
	else
		[[self window] setTitle: t];
}

- (void) applicationSetTitle: (NSString *)t
{
	[oscTitle release];
	oscTitle = nil;
	if ([t length] > 0) {
		oscTitle = [t retain];
		oscPgrp = term != NULL && rome_term_fd(term) >= 0 ? tcgetpgrp(rome_term_fd(term)) : 0;
		[self showTitle: t];
	} else {
		[self pollTitle: nil];
	}
}

- (void) pollTitle: (NSTimer *)timer
{
	if (term == NULL || rome_term_fd(term) < 0)
		return;
	pid_t pg = tcgetpgrp(rome_term_fd(term));
	if (pg <= 0)
		return;
	if (oscTitle != nil && pg != oscPgrp) {
		[oscTitle release];
		oscTitle = nil;
	}
	if (oscTitle != nil)
		return;
	if (pg != fgPgrp || fgName == nil) {
		fgPgrp = pg;
		[fgName release];
		fgName = [process_name(pg) retain];
	}
	[self showTitle: fgName];
}

/* Shown or hidden as the window's selected tab. A hidden tab keeps reading its
 * pty but draws nothing, and its X child is unmapped. */
- (void) setTabVisible: (BOOL)visible
{
	if (visible == !tabHidden)
		return;
	tabHidden = !visible;
	[self setHidden: tabHidden];
	if (renderer != NULL)
		rome_render_show(renderer, visible);
	if (term != NULL)
		rome_term_set_focus(term, visible && [[self window] isKeyWindow]);
	if (visible && renderer != NULL) {
		[self layoutGrid];
		rome_term_damage_all(term);
		[self scheduleRender];
	}
}

/* ROME_STATS: a 20 ms timer; a late firing is a run loop that was busy or
 * blocked, which is what makes typing stall. */
- (void) probe: (NSTimer *)t
{
	double now = rome_now_ms();
	if (now - probeLast > 100)
		fprintf(stderr, "rome: run loop stall %.0fms\n", now - probeLast - 20);
	probeLast = now;
}

- (void) windowResized: (NSNotification *)n
{
	[self layoutGrid];
}

- (void) windowWillClose: (NSNotification *)n
{
	[self shutdown];
}

- (void) stopReading
{
	if (!reading)
		return;
	NSRunLoop *rl = [NSRunLoop currentRunLoop];
	void *fd = (void *)(intptr_t)rome_term_fd(term);
	[rl removeEvent: fd type: ET_RDESC forMode: NSDefaultRunLoopMode all: YES];
	[rl removeEvent: fd type: ET_RDESC forMode: NSEventTrackingRunLoopMode all: YES];
	if (writing)
		[rl removeEvent: fd type: ET_WDESC forMode: NSDefaultRunLoopMode all: YES];
	reading = writing = NO;
}

- (void) shutdown
{
	[[NSNotificationCenter defaultCenter] removeObserver: self];
	[NSObject cancelPreviousPerformRequestsWithTarget: self];
	[blinkTimer invalidate];
	[blinkTimer release];
	blinkTimer = nil;
	[probeTimer invalidate];
	[probeTimer release];
	probeTimer = nil;
	[titleTimer invalidate];
	[titleTimer release];
	titleTimer = nil;
	if (term != NULL)
		[self stopReading];
	/* destroy the child X window before GNUstep destroys its parent */
	if (renderer != NULL) {
		renderer->destroy(renderer);
		renderer = NULL;
	}
	if (term != NULL) {
		rome_term_free(term);
		term = NULL;
	}
	if (font != NULL) {
		rome_font_free(font);
		font = NULL;
	}
}

/* ---- pty I/O ---- */

- (void) receivedEvent: (void *)data type: (RunLoopEventType)type extra: (void *)extra forMode: (NSString *)mode
{
	if (term == NULL)
		return;
	if (type == ET_WDESC) {
		if (!rome_term_write_pending(term)) {
			[[NSRunLoop currentRunLoop] removeEvent: data type: ET_WDESC forMode: NSDefaultRunLoopMode all: YES];
			writing = NO;
		}
		return;
	}
	/* Up to 256 KiB per wake-up; a flood of output is drawn at most
	 * RomeMaxFPS times a second (by the frame timer), a trickle at once. */
	long n = rome_term_read(term, 256 * 1024);
	if (n > 0) {
		bytesRead += n;
		/* Recent output volume, halving every 100 ms: a flood arrives in small
		 * pty-sized reads, typing and prompts barely register. */
		double now = rome_now_ms();
		recentBytes = recentBytes * exp2(-(now - recentAt) / 100.0) + n;
		recentAt = now;
		if (immediateRender && renderer != NULL && recentBytes <= 4096) {
			/* Interactive output (an echo, a prompt): draw it now rather than
			 * waiting on the frame timer, which can fire up to a hundred
			 * milliseconds late for a background-class process. A flood keeps
			 * the frame cap, since each small read would otherwise cost a
			 * frame. */
			if (renderScheduled) {
				[NSObject cancelPreviousPerformRequestsWithTarget: self selector: @selector(renderNow) object: nil];
				renderScheduled = NO;
			}
			[self renderNow];
		} else {
			[self scheduleRender];
		}
	} else if (n < 0) {
		[self childExited];
	}
}

- (void) startWriting
{
	if (writing || rome_term_fd(term) < 0)
		return;
	[[NSRunLoop currentRunLoop] addEvent: (void *)(intptr_t)rome_term_fd(term) type: ET_WDESC
	    watcher: self forMode: NSDefaultRunLoopMode];
	writing = YES;
}

- (void) childExited
{
	int status = 0;
	pid_t pid = rome_term_pid(term);
	[self stopReading];
	exited = YES;
	if (pid > 0)
		waitpid(pid, &status, 0);
	const char *msg = "\r\n[Process completed]";
	rome_term_feed(term, msg, strlen(msg));
	[self scheduleRender];
	if (g_stats)
		[self printStats];
	[[NSNotificationCenter defaultCenter] postNotificationName: RomeViewChildExitedNotification object: self
	    userInfo: [NSDictionary dictionaryWithObject: [NSNumber numberWithInt: status] forKey: @"status"]];
}

/* ---- frames ---- */

- (void) scheduleRender
{
	if (renderScheduled || renderer == NULL)
		return;
	renderScheduled = YES;
	double now = rome_now_ms();
	double delay = lastRenderMs + minFrameMs - now;
	if (delay < 0)
		delay = 0;
	[self performSelector: @selector(renderNow) withObject: nil afterDelay: delay / 1000.0
	    inModes: [NSArray arrayWithObjects: NSDefaultRunLoopMode, NSEventTrackingRunLoopMode,
	                 NSModalPanelRunLoopMode, nil]];
}

- (void) renderNow
{
	renderScheduled = NO;
	if (renderer == NULL || term == NULL || tabHidden)
		return;
	if (rome_term_needs_render(term)) {
		double t0 = rome_now_ms();
		rowsDrawn += rome_term_render(term, renderer);
		double t1 = rome_now_ms();
		renderMs += t1 - t0;
		frames++;
		lastRenderMs = t1;
		if (keyTimeMs > 0) {
			double l = t1 - keyTimeMs;
			latencySumMs += l;
			if (l > latencyMaxMs)
				latencyMaxMs = l;
			latencySamples++;
			keyTimeMs = 0;
		}
		if (g_stats && frames == 1)
			fprintf(stderr, "rome: first frame drawn (%d rows)\n", (int)rowsDrawn);
		if (g_stats && frames % 200 == 0)
			[self printStats];
	}
	[self updateScroller];
	/* XSync in the frame may have queued events (exposes) */
	rome_x_pump();
}

- (void) printStats
{
	if (frames == 0 || renderer == NULL)
		return;
	fprintf(stderr, "rome-stats: renderer=%s window=%dx%d grid=%dx%d frames=%lu rows/frame=%.1f "
	    "frame=%.2fms (draw %.2f, present %.2f) px/frame=%lu bytes=%ld wall=%.0fms",
	    renderer->name, renderer->width, renderer->height, rome_term_cols(term), rome_term_rows(term),
	    frames, (double)rowsDrawn / frames, renderMs / frames, renderer->t_draw / frames,
	    renderer->t_present / frames, renderer->px_present / frames, bytesRead, rome_now_ms() - startMs);
	if (latencySamples)
		fprintf(stderr, " key->screen avg=%.2fms max=%.2fms n=%lu", latencySumMs / latencySamples,
		    latencyMaxMs, latencySamples);
	fprintf(stderr, "\n");
}

- (void) exposeX: (NSValue *)v
{
	NSRect r = [v rectValue];
	if (term == NULL)
		return;
	rome_term_expose(term, (int)r.origin.x, (int)r.origin.y, (int)r.size.width, (int)r.size.height);
	[self scheduleRender];
}

- (void) blink: (NSTimer *)t
{
	static int phase = 1;
	if (term == NULL || !rome_term_cursor_blinks(term))
		return;
	phase = !phase;
	rome_term_set_cursor_phase(term, phase);
	[self scheduleRender];
}

- (void) cursorActivity
{
	if (term == NULL)
		return;
	rome_term_set_cursor_phase(term, 1);
	if (keyTimeMs == 0)
		keyTimeMs = rome_now_ms();
	/* restart the blink cycle so the cursor stays on while typing */
	if (blinkTimer != nil)
		[blinkTimer setFireDate: [NSDate dateWithTimeIntervalSinceNow: 0.53]];
}

/* ---- keyboard ---- */

- (int) keyMods: (NSEvent *)ev
{
	unsigned f = [ev modifierFlags];
	int m = 0;
	if (f & NSShiftKeyMask) m |= ROME_MOD_SHIFT;
	if (f & NSControlKeyMask) m |= ROME_MOD_CTRL;
	if ((f & NSAlternateKeyMask) && optionAsMeta) m |= ROME_MOD_ALT;
	return m;
}

- (void) keyDown: (NSEvent *)ev
{
	if (term == NULL || exited)
		return;
	unsigned flags = [ev modifierFlags];
	if (flags & NSCommandKeyMask)
		return;             /* menu key equivalents only */
	NSString *chars = [ev characters], *raw = [ev charactersIgnoringModifiers];
	int mods = [self keyMods: ev];
	unichar c = [raw length] ? [raw characterAtIndex: 0] : ([chars length] ? [chars characterAtIndex: 0] : 0);
	int key = ROME_KEY_NONE;
	int rows = rome_term_rows(term);
	switch (c) {
	case NSUpArrowFunctionKey: key = ROME_KEY_UP; break;
	case NSDownArrowFunctionKey: key = ROME_KEY_DOWN; break;
	case NSLeftArrowFunctionKey: key = ROME_KEY_LEFT; break;
	case NSRightArrowFunctionKey: key = ROME_KEY_RIGHT; break;
	case NSHomeFunctionKey:
		if (mods & ROME_MOD_SHIFT) { rome_term_scroll_view(term, 1 << 30); [self scheduleRender]; return; }
		key = ROME_KEY_HOME; break;
	case NSEndFunctionKey:
		if (mods & ROME_MOD_SHIFT) { rome_term_scroll_to_bottom(term); [self scheduleRender]; return; }
		key = ROME_KEY_END; break;
	case NSPageUpFunctionKey:
		if (mods & ROME_MOD_SHIFT) { rome_term_scroll_view(term, rows - 1); [self scheduleRender]; return; }
		key = ROME_KEY_PAGEUP; break;
	case NSPageDownFunctionKey:
		if (mods & ROME_MOD_SHIFT) { rome_term_scroll_view(term, -(rows - 1)); [self scheduleRender]; return; }
		key = ROME_KEY_PAGEDOWN; break;
	case NSInsertFunctionKey: key = ROME_KEY_INS; break;
	case NSDeleteFunctionKey: key = ROME_KEY_DEL; break;
	case '\r': case 3: key = ROME_KEY_ENTER; break;
	case '\t': case 0x19: key = ROME_KEY_TAB; break;
	case 0x7f: case 8: key = ROME_KEY_BACKSPACE; break;
	case 0x1b: key = ROME_KEY_ESCAPE; break;
	default:
		if (c >= NSF1FunctionKey && c <= NSF35FunctionKey) {
			key = ROME_KEY_F((int)(c - NSF1FunctionKey + 1));
		}
		break;
	}
	[self cursorActivity];
	[NSCursor setHiddenUntilMouseMoves: YES];
	if (rome_term_has_selection(term) && !(mods & ROME_MOD_SHIFT))
		rome_term_select_clear(term);
	if (key != ROME_KEY_NONE) {
		if (c == 0x19)
			mods |= ROME_MOD_SHIFT;
		rome_term_key(term, key, mods);
		[self scheduleRender];
		return;
	}
	/* With Control or Meta the key's own character (Ctrl-C is 'c');
	 * otherwise the composed text. */
	NSString *s = (mods & (ROME_MOD_CTRL | ROME_MOD_ALT)) ? raw : chars;
	NSUInteger n = [s length];
	for (NSUInteger i = 0; i < n; i++) {
		uint32_t u = [s characterAtIndex: i];
		if (u >= 0xd800 && u < 0xdc00 && i + 1 < n) {
			uint32_t lo = [s characterAtIndex: i + 1];
			if (lo >= 0xdc00 && lo < 0xe000) {
				u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
				i++;
			}
		}
		if (u >= 0xf700 && u <= 0xf8ff)
			continue;       /* other function keys */
		rome_term_key_char(term, u, mods);
	}
	[self scheduleRender];
}

- (void) sendBenchKey: (unichar)c
{
	if (term == NULL || exited)
		return;
	[self cursorActivity];
	char b = (char)c;
	rome_term_send_raw(term, &b, 1);
}

/* ---- mouse ---- */

- (void) cellAt: (NSEvent *)ev row: (int *)row col: (int *)col clamp: (BOOL)clamp
{
	NSPoint p = [self convertPoint: [ev locationInWindow] fromView: nil];
	int c = (int)((p.x - pad) / font->cell_w), r = (int)((p.y - pad) / font->cell_h);
	if (p.x < pad) c = 0;
	if (p.y < pad) r = clamp ? 0 : -1;
	if (clamp) {
		int rows = rome_term_rows(term), cols = rome_term_cols(term);
		if (c >= cols) c = cols - 1;
		if (r >= rows) r = rows - 1;
		if (r < 0) r = 0;
		if (c < 0) c = 0;
	}
	*row = r;
	*col = c;
}

- (BOOL) reportMouse: (NSEvent *)ev
{
	return term != NULL && rome_term_mouse_mode(term) != 0 && !([ev modifierFlags] & NSShiftKeyMask);
}

- (void) mouseEvent: (NSEvent *)ev button: (int)button pressed: (int)pressed
{
	int row, col;
	[self cellAt: ev row: &row col: &col clamp: YES];
	rome_term_mouse(term, row, col, button, pressed, [self keyMods: ev] & ~ROME_MOD_SHIFT);
}

- (void) mouseDown: (NSEvent *)ev
{
	if (term == NULL)
		return;
	[[self window] makeFirstResponder: self];
	if ([self reportMouse: ev]) {
		mouseReporting = YES;
		[self mouseEvent: ev button: 1 pressed: 1];
		return;
	}
	mouseReporting = NO;
	int row, col;
	[self cellAt: ev row: &row col: &col clamp: YES];
	selClicks = (int)[ev clickCount];
	selecting = YES;
	rome_term_select_begin(term, row, col, selClicks);
	[self scheduleRender];
}

- (void) mouseDragged: (NSEvent *)ev
{
	if (term == NULL)
		return;
	if (mouseReporting) {
		if (rome_term_mouse_mode(term) >= ROME_MOUSE_DRAG)
			[self mouseEvent: ev button: 0 pressed: 0];
		return;
	}
	if (!selecting)
		return;
	int row, col;
	[self cellAt: ev row: &row col: &col clamp: YES];
	rome_term_select_extend(term, row, col);
	[self scheduleRender];
}

- (void) mouseUp: (NSEvent *)ev
{
	if (term == NULL)
		return;
	if (mouseReporting) {
		[self mouseEvent: ev button: 1 pressed: 0];
		mouseReporting = NO;
		return;
	}
	selecting = NO;
}

- (void) rightMouseDown: (NSEvent *)ev
{
	if ([self reportMouse: ev])
		[self mouseEvent: ev button: 3 pressed: 1];
	else
		[super rightMouseDown: ev];
}

- (void) rightMouseUp: (NSEvent *)ev
{
	if ([self reportMouse: ev])
		[self mouseEvent: ev button: 3 pressed: 0];
}

- (void) scrollWheel: (NSEvent *)ev
{
	if (term == NULL)
		return;
	CGFloat dy = [ev deltaY];
	int steps = dy > 0 ? (int)(dy + 0.5) : (int)(dy - 0.5);
	if (steps == 0)
		steps = dy > 0 ? 1 : -1;
	if ([self reportMouse: ev]) {
		for (int i = 0; i < abs(steps); i++) {
			[self mouseEvent: ev button: steps > 0 ? 4 : 5 pressed: 1];
		}
		return;
	}
	if (rome_term_altscreen(term)) {
		/* less, vim, man: the wheel moves the text */
		for (int i = 0; i < 3 * abs(steps); i++)
			rome_term_key(term, steps > 0 ? ROME_KEY_UP : ROME_KEY_DOWN, 0);
		return;
	}
	rome_term_scroll_view(term, 3 * steps);
	[self scheduleRender];
}

/* ---- pasteboard and menu actions ---- */

- (void) copy: (id)sender
{
	char *s = term ? rome_term_selection_text(term) : NULL;
	if (s == NULL)
		return;
	NSPasteboard *pb = [NSPasteboard generalPasteboard];
	[pb declareTypes: [NSArray arrayWithObject: NSStringPboardType] owner: nil];
	[pb setString: [NSString stringWithUTF8String: s] forType: NSStringPboardType];
	free(s);
}

- (void) paste: (id)sender
{
	NSString *s = [[NSPasteboard generalPasteboard] stringForType: NSStringPboardType];
	if (s == nil || term == NULL || exited)
		return;
	const char *u = [s UTF8String];
	rome_term_paste(term, u, strlen(u));
	[self cursorActivity];
	[self scheduleRender];
}

- (void) selectAll: (id)sender
{
	if (term == NULL)
		return;
	rome_term_select_all(term);
	[self scheduleRender];
}

- (void) clearScrollback: (id)sender
{
	if (term == NULL)
		return;
	/* ED 3 (xterm): erase the saved lines */
	rome_term_feed(term, "\033[3J", 4);
	[self scheduleRender];
}

- (BOOL) validateMenuItem: (id)item
{
	SEL a = [item action];
	if (a == @selector(copy:))
		return term != NULL && rome_term_has_selection(term);
	if (a == @selector(paste:))
		return term != NULL && !exited &&
		    [[NSPasteboard generalPasteboard] availableTypeFromArray: [NSArray arrayWithObject: NSStringPboardType]] != nil;
	return YES;
}

@end
