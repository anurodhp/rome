/*
 * rome: the terminal view. An NSView whose pixels are a child X window drawn
 * by a RomeRenderer (RomeX.h says why); it handles input, selection, the
 * pasteboard and the frame timing, and owns the RomeTerm.
 */
#import <AppKit/AppKit.h>
#include "RomeTerm.h"

@interface RomeView : NSView <RunLoopEvents>
{
	RomeTerm *term;
	RomeRenderer *renderer;
	RomeFont *font;
	NSString *rendererName;
	NSArray *command;
	int pad;
	double minFrameMs, lastRenderMs;
	BOOL renderScheduled, reading, writing, exited, optionAsMeta, blinkEnabled;
	NSTimer *blinkTimer;
	int selRow, selCol, selClicks;
	BOOL selecting, mouseReporting;
	/* stats (ROME_STATS) */
	unsigned long frames, rowsDrawn;
	double renderMs, keyTimeMs, latencySumMs, latencyMaxMs;
	unsigned long latencySamples;
	double startMs;
	long bytesRead;
}
- (id) initWithFrame: (NSRect)frame command: (NSArray *)argv;
+ (NSSize) contentSizeForColumns: (int)cols rows: (int)rows;
- (NSSize) cellSize;
- (void) shutdown;
- (const char *) rendererName;
- (void) sendBenchKey: (unichar)c;
- (void) printStats;
@end

/* posted when the child exits; object = the view, userInfo "status" */
extern NSString *RomeViewChildExitedNotification;
