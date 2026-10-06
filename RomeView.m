/*
 * rome: the terminal view (see RomeView.h).
 */
#import "RomeView.h"
#import <GNUstepGUI/GSDisplayServer.h>
#include "RomeX.h"
#include <vterm.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

static BOOL g_stats;

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
	RomeView *v = owner;
	NSString *s = [NSString stringWithUTF8String: utf8];
	if (s != nil && [v window] != nil)
		[[v window] setTitle: s];
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
	if (f == NULL)
		return NSMakeSize(cols * 8 + 2 * pad, rows * 16 + 2 * pad);
	return NSMakeSize(cols * f->cell_w + 2 * pad, rows * f->cell_h + 2 * pad);
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
	rendererName = [default_string(@"RomeRenderer", @"x11") retain];
	NSString *th = default_string(@"RomeTheme", @"Basic");
	const RomeTheme *theme = [th caseInsensitiveCompare: @"Pro"] == NSOrderedSame ? &theme_pro : &theme_basic;

	RomeTermCallbacks cb = { title_cb, bell_cb, want_write_cb };
	int cols = ((int)frame.size.width - 2 * pad) / font->cell_w;
	int rows = ((int)frame.size.height - 2 * pad) / font->cell_h;
	term = rome_term_new(rows, cols, (int)default_double(@"RomeScrollback", 5000), theme, &cb, self);
	if (term == NULL) {
		[self release];
		return nil;
	}
	[self setAutoresizingMask: NSViewWidthSizable | NSViewHeightSizable];
	startMs = rome_now_ms();
	return self;
}

- (void) dealloc
{
	[self shutdown];
	[command release];
	[rendererName release];
	[super dealloc];
}

- (BOOL) isFlipped { return YES; }
- (BOOL) isOpaque { return YES; }
- (BOOL) acceptsFirstResponder { return YES; }
- (BOOL) becomeFirstResponder { return YES; }
- (NSSize) cellSize { return NSMakeSize(font->cell_w, font->cell_h); }
- (const char *) rendererName { return renderer ? renderer->name : "none"; }

- (void) drawRect: (NSRect)r
{
	/* covered by the child window; shows only while it is being created */
	[[NSColor windowBackgroundColor] set];
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
	NSSize s = [self bounds].size;
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
	NSString *th = default_string(@"RomeTheme", @"Basic");
	renderer->padbg = [th caseInsensitiveCompare: @"Pro"] == NSOrderedSame ? theme_pro.bg : theme_basic.bg;
}

- (void) layoutGrid
{
	if (renderer == NULL)
		return;
	NSSize s = [self bounds].size;
	NSPoint o = [self xOrigin];
	rome_render_move(renderer, (int)o.x, (int)o.y);
	renderer->resize(renderer, (int)s.width, (int)s.height);
	int cols = ((int)s.width - 2 * pad) / font->cell_w;
	int rows = ((int)s.height - 2 * pad) / font->cell_h;
	rome_term_resize(term, rows, cols, cols * font->cell_w, rows * font->cell_h);
	rome_term_damage_all(term);
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
	rome_term_set_focus(term, [[self window] isKeyWindow]);
	[self scheduleRender];
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
	/* Up to 256 KiB per wake-up; the frame timer runs between wake-ups, so
	 * a flood of output is drawn at most RomeMaxFPS times a second. */
	long n = rome_term_read(term, 256 * 1024);
	if (n > 0) {
		bytesRead += n;
		[self scheduleRender];
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
	if (renderer == NULL || term == NULL)
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

- (int) vtermMods: (NSEvent *)ev
{
	unsigned f = [ev modifierFlags];
	int m = 0;
	if (f & NSShiftKeyMask) m |= VTERM_MOD_SHIFT;
	if (f & NSControlKeyMask) m |= VTERM_MOD_CTRL;
	if ((f & NSAlternateKeyMask) && optionAsMeta) m |= VTERM_MOD_ALT;
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
	int mods = [self vtermMods: ev];
	unichar c = [raw length] ? [raw characterAtIndex: 0] : ([chars length] ? [chars characterAtIndex: 0] : 0);
	int key = VTERM_KEY_NONE;
	int rows = rome_term_rows(term);
	switch (c) {
	case NSUpArrowFunctionKey: key = VTERM_KEY_UP; break;
	case NSDownArrowFunctionKey: key = VTERM_KEY_DOWN; break;
	case NSLeftArrowFunctionKey: key = VTERM_KEY_LEFT; break;
	case NSRightArrowFunctionKey: key = VTERM_KEY_RIGHT; break;
	case NSHomeFunctionKey:
		if (mods & VTERM_MOD_SHIFT) { rome_term_scroll_view(term, 1 << 30); [self scheduleRender]; return; }
		key = VTERM_KEY_HOME; break;
	case NSEndFunctionKey:
		if (mods & VTERM_MOD_SHIFT) { rome_term_scroll_to_bottom(term); [self scheduleRender]; return; }
		key = VTERM_KEY_END; break;
	case NSPageUpFunctionKey:
		if (mods & VTERM_MOD_SHIFT) { rome_term_scroll_view(term, rows - 1); [self scheduleRender]; return; }
		key = VTERM_KEY_PAGEUP; break;
	case NSPageDownFunctionKey:
		if (mods & VTERM_MOD_SHIFT) { rome_term_scroll_view(term, -(rows - 1)); [self scheduleRender]; return; }
		key = VTERM_KEY_PAGEDOWN; break;
	case NSInsertFunctionKey: key = VTERM_KEY_INS; break;
	case NSDeleteFunctionKey: key = VTERM_KEY_DEL; break;
	case '\r': case 3: key = VTERM_KEY_ENTER; break;
	case '\t': case 0x19: key = VTERM_KEY_TAB; break;
	case 0x7f: case 8: key = VTERM_KEY_BACKSPACE; break;
	case 0x1b: key = VTERM_KEY_ESCAPE; break;
	default:
		if (c >= NSF1FunctionKey && c <= NSF35FunctionKey) {
			key = VTERM_KEY_FUNCTION((int)(c - NSF1FunctionKey + 1));
		}
		break;
	}
	[self cursorActivity];
	[NSCursor setHiddenUntilMouseMoves: YES];
	if (rome_term_has_selection(term) && !(mods & VTERM_MOD_SHIFT))
		rome_term_select_clear(term);
	if (key != VTERM_KEY_NONE) {
		if (c == 0x19)
			mods |= VTERM_MOD_SHIFT;
		rome_term_key(term, key, mods);
		[self scheduleRender];
		return;
	}
	/* With Control or Meta the key's own character (Ctrl-C is 'c');
	 * otherwise the composed text. */
	NSString *s = (mods & (VTERM_MOD_CTRL | VTERM_MOD_ALT)) ? raw : chars;
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
	rome_term_mouse(term, row, col, button, pressed, [self vtermMods: ev] & ~VTERM_MOD_SHIFT);
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
	selRow = row;
	selCol = col;
	selecting = YES;
	if (selClicks == 2)
		rome_term_select_word(term, row, col);
	else if (selClicks >= 3)
		rome_term_select_line(term, row);
	else
		rome_term_select_clear(term);
	[self scheduleRender];
}

- (void) mouseDragged: (NSEvent *)ev
{
	if (term == NULL)
		return;
	if (mouseReporting) {
		if (rome_term_mouse_mode(term) >= VTERM_PROP_MOUSE_DRAG)
			[self mouseEvent: ev button: 0 pressed: 0];
		return;
	}
	if (!selecting)
		return;
	int row, col;
	[self cellAt: ev row: &row col: &col clamp: YES];
	rome_term_select(term, selRow, selCol, row, col);
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
			rome_term_key(term, steps > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, 0);
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
	/* ED 3 (xterm): erase the saved lines; libvterm calls sb_clear */
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
