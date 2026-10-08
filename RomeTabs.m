/*
 * rome: tabs (see RomeTabs.h).
 */
#import "RomeTabs.h"

static const CGFloat kBarH = 26, kPlusW = 30, kMaxTabW = 220, kCloseW = 24;

static NSColor *
rgb(unsigned c)
{
	return [NSColor colorWithCalibratedRed: ((c >> 16) & 255) / 255.0 green: ((c >> 8) & 255) / 255.0
	    blue: (c & 255) / 255.0 alpha: 1];
}

/* ---- the strip of tab titles ---- */

@interface RomeTabBar : NSView
{
	RomeWindowController *owner;
}
- (void) setOwner: (RomeWindowController *)o;
@end

@implementation RomeTabBar
- (void) setOwner: (RomeWindowController *)o { owner = o; }
- (BOOL) isFlipped { return YES; }
- (BOOL) acceptsFirstMouse: (NSEvent *)e { return YES; }

- (CGFloat) tabWidth
{
	NSUInteger n = [owner tabCount];
	if (n == 0)
		return kMaxTabW;
	CGFloat w = ([self bounds].size.width - kPlusW) / n;
	return w > kMaxTabW ? kMaxTabW : w;
}

- (void) drawRect: (NSRect)r
{
	NSRect b = [self bounds];
	[rgb(0x1c1c1c) set];
	NSRectFill(b);
	NSUInteger n = [owner tabCount], sel = [owner selectedIndex];
	CGFloat w = [self tabWidth];
	NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle new] autorelease];
	[ps setLineBreakMode: NSLineBreakByTruncatingTail];
	for (NSUInteger i = 0; i < n; i++) {
		NSRect t = NSMakeRect(i * w, 0, w, b.size.height);
		BOOL on = i == sel;
		[(on ? rgb(0x000000) : rgb(0x2b2b2b)) set];
		NSRectFill(t);
		[rgb(0x3d3d3d) set];
		NSRectFill(NSMakeRect(NSMaxX(t) - 1, 0, 1, b.size.height));
		NSDictionary *a = [NSDictionary dictionaryWithObjectsAndKeys:
		    [NSFont systemFontOfSize: 11], NSFontAttributeName,
		    on ? rgb(0xffffff) : rgb(0x9a9a9a), NSForegroundColorAttributeName,
		    ps, NSParagraphStyleAttributeName, nil];
		NSRect tr = NSMakeRect(t.origin.x + 10, 6, w - 10 - kCloseW, 16);
		[[owner titleAtIndex: i] drawInRect: tr withAttributes: a];
		[@"×" drawAtPoint: NSMakePoint(NSMaxX(t) - kCloseW + 6, 5) withAttributes: a];
	}
	NSDictionary *a = [NSDictionary dictionaryWithObjectsAndKeys:
	    [NSFont systemFontOfSize: 15], NSFontAttributeName, rgb(0x9a9a9a), NSForegroundColorAttributeName, nil];
	[@"+" drawAtPoint: NSMakePoint(n * w + 10, 3) withAttributes: a];
}

- (void) mouseDown: (NSEvent *)ev
{
	NSPoint p = [self convertPoint: [ev locationInWindow] fromView: nil];
	NSUInteger n = [owner tabCount];
	CGFloat w = [self tabWidth];
	if (p.x < 0)
		return;
	NSUInteger i = (NSUInteger)(p.x / w);
	if (i < n) {
		if (p.x > (i + 1) * w - kCloseW)
			[owner closeIndex: i];
		else
			[owner selectIndex: i];
	} else if (p.x < n * w + kPlusW) {
		[owner newTab: nil];
	}
}
@end

/* ---- the window's content view ---- */

@interface RomeTabsView : NSView
{
	RomeWindowController *owner;
}
- (void) setOwner: (RomeWindowController *)o;
@end

@implementation RomeTabsView
- (void) setOwner: (RomeWindowController *)o { owner = o; }
- (BOOL) isFlipped { return YES; }
- (BOOL) isOpaque { return YES; }
- (void) drawRect: (NSRect)r
{
	[rgb(0x1c1c1c) set];
	NSRectFill(r);
}
- (void) setFrameSize: (NSSize)s
{
	[super setFrameSize: s];
	[owner layoutTabs];
}
@end

/* ---- the controller ---- */

@implementation RomeWindowController

- (id) initWithWindow: (NSWindow *)w command: (NSArray *)argv
{
	self = [super init];
	if (self == nil)
		return nil;
	window = w;
	command = [argv retain];
	tabs = [NSMutableArray new];
	NSRect fr = [[w contentView] frame];
	container = [[RomeTabsView alloc] initWithFrame: fr];
	[(RomeTabsView *)container setOwner: self];
	bar = [[RomeTabBar alloc] initWithFrame: NSMakeRect(0, 0, fr.size.width, kBarH)];
	[(RomeTabBar *)bar setOwner: self];
	[bar setAutoresizingMask: NSViewWidthSizable];
	[bar setHidden: YES];
	[container addSubview: bar];
	[w setContentView: container];
	[w setDelegate: self];
	return self;
}

- (void) dealloc
{
	[container release];
	[bar release];
	[tabs release];
	[command release];
	[super dealloc];
}

- (NSWindow *) window { return window; }
- (RomeView *) selectedView { return selected; }
- (NSUInteger) tabCount { return [tabs count]; }

- (NSUInteger) selectedIndex
{
	NSUInteger i = selected ? [tabs indexOfObjectIdenticalTo: selected] : NSNotFound;
	return i == NSNotFound ? 0 : i;
}

- (NSString *) titleAtIndex: (NSUInteger)i
{
	return i < [tabs count] ? [[tabs objectAtIndex: i] title] : @"";
}

- (void) layoutTabs
{
	NSRect b = [container bounds];
	BOOL showBar = [tabs count] > 1;
	CGFloat bh = showBar ? kBarH : 0;
	if ([bar isHidden] == showBar)
		[bar setHidden: !showBar];
	[bar setFrame: NSMakeRect(0, 0, b.size.width, kBarH)];
	NSRect vf = NSMakeRect(0, bh, b.size.width, b.size.height - bh);
	for (RomeView *v in tabs) {
		if (!NSEqualRects([v frame], vf))
			[v setFrame: vf];
	}
	[container setNeedsDisplay: YES];
	[bar setNeedsDisplay: YES];
}

- (RomeView *) addTab
{
	NSRect b = [container bounds];
	RomeView *v = [[RomeView alloc] initWithFrame: NSMakeRect(0, 0, b.size.width, b.size.height) command: command];
	if (v == nil)
		return nil;
	[v setController: self];
	[container addSubview: v];
	[tabs addObject: v];
	[v release];
	[self layoutTabs];
	[self selectIndex: [tabs count] - 1];
	return v;
}

- (void) selectIndex: (NSUInteger)i
{
	if (i >= [tabs count])
		return;
	RomeView *v = [tabs objectAtIndex: i];
	for (RomeView *t in tabs) {
		if (t != v)
			[t setTabVisible: NO];
	}
	selected = v;
	[v setTabVisible: YES];
	[window setTitle: [v title]];
	[window makeFirstResponder: v];
	[bar setNeedsDisplay: YES];
}

- (void) closeView: (RomeView *)v
{
	NSUInteger i = [tabs indexOfObjectIdenticalTo: v];
	if (i == NSNotFound)
		return;
	[v retain];
	[tabs removeObjectAtIndex: i];
	[v shutdown];
	[v removeFromSuperview];
	[v release];
	if ([tabs count] == 0) {
		selected = nil;
		[window close];
		return;
	}
	[self layoutTabs];
	if (v == selected) {
		selected = nil;
		[self selectIndex: i < [tabs count] ? i : [tabs count] - 1];
	}
	[bar setNeedsDisplay: YES];
}

- (void) closeIndex: (NSUInteger)i
{
	if (i < [tabs count])
		[self closeView: [tabs objectAtIndex: i]];
}

- (void) newTab: (id)sender
{
	[[self addTab] start];
}

- (void) closeTab: (id)sender
{
	if (selected != nil)
		[self closeView: selected];
}

- (void) nextTab: (id)sender
{
	if ([tabs count] > 1)
		[self selectIndex: ([self selectedIndex] + 1) % [tabs count]];
}

- (void) previousTab: (id)sender
{
	if ([tabs count] > 1)
		[self selectIndex: ([self selectedIndex] + [tabs count] - 1) % [tabs count]];
}

- (void) romeView: (RomeView *)v titleChanged: (NSString *)t
{
	if (v == selected)
		[window setTitle: t];
	[bar setNeedsDisplay: YES];
}

@end
