/*
 * rome: its own X connection, and the child windows the renderers draw in.
 *
 * Why not draw through the GNUstep window: gnustep-back hands a child
 * window's Expose events to nobody (XGServerEvent.m, the Expose case: the
 * sub-window lookup is commented out, so an expose of a window it does not
 * know is dropped), and NSOpenGLView's -flushBuffer is glXSwapBuffers, which
 * puts the whole window. rome needs both: exposes, and putting only the
 * pixels that changed. So it opens one X connection of its own, creates its
 * child windows on it (inside GNUstep's window; X allows a child of another
 * client's window), and reads that connection's events itself.
 *
 * Mouse and keyboard events are not selected on the child, so X delivers
 * them to the GNUstep window as before and AppKit handles input normally.
 */
#ifndef ROME_X_H
#define ROME_X_H

#include <X11/Xlib.h>

Display *rome_x_display(void);

/* Called for Expose / GraphicsExpose on a registered window. */
typedef void (*RomeExposeFn)(void *owner, int x, int y, int w, int h);
void rome_x_register(Window w, RomeExposeFn fn, void *owner);
void rome_x_unregister(Window w);

/* Handle every queued event. Call when the connection's fd is readable and
 * after anything that may have read events into Xlib's queue (XSync). */
void rome_x_pump(void);

#endif
