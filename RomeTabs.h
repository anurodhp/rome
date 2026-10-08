/*
 * rome: tabs. A window holds one RomeView per tab, all the same size in a
 * container; the selected one is shown and the others are hidden, which for
 * rome also unmaps their X child windows (an X child draws over AppKit).
 */
#import <AppKit/AppKit.h>
#import "RomeView.h"

@interface RomeWindowController : NSObject
{
	NSWindow *window;
	NSView *container;
	NSView *bar;
	NSMutableArray *tabs;
	NSArray *command;
	RomeView *selected;
}
- (id) initWithWindow: (NSWindow *)w command: (NSArray *)argv;
- (RomeView *) addTab;
- (RomeView *) selectedView;
- (NSUInteger) tabCount;
- (NSUInteger) selectedIndex;
- (NSString *) titleAtIndex: (NSUInteger)i;
- (void) selectIndex: (NSUInteger)i;
- (void) closeIndex: (NSUInteger)i;
- (void) closeView: (RomeView *)v;
- (void) newTab: (id)sender;
- (void) closeTab: (id)sender;
- (void) nextTab: (id)sender;
- (void) previousTab: (id)sender;
- (void) romeView: (RomeView *)v titleChanged: (NSString *)t;
- (void) layoutTabs;
- (NSWindow *) window;
@end
