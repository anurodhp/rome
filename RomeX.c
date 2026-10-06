/*
 * rome: its own X connection (see RomeX.h).
 */
#include "RomeX.h"
#include "RomeRender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Display *g_dpy;

typedef struct {
	Window win;
	RomeExposeFn fn;
	void *owner;
} Reg;
static Reg *g_regs;
static int g_nregs, g_capregs;

double
rome_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

Display *
rome_x_display(void)
{
	if (g_dpy == NULL) {
		g_dpy = XOpenDisplay(NULL);
		if (g_dpy == NULL)
			fprintf(stderr, "rome: cannot open the X display\n");
	}
	return g_dpy;
}

void
rome_x_register(Window w, RomeExposeFn fn, void *owner)
{
	if (g_nregs == g_capregs) {
		g_capregs = g_capregs ? 2 * g_capregs : 8;
		g_regs = realloc(g_regs, g_capregs * sizeof(*g_regs));
	}
	g_regs[g_nregs++] = (Reg){ w, fn, owner };
}

void
rome_x_unregister(Window w)
{
	for (int i = 0; i < g_nregs; i++) {
		if (g_regs[i].win == w) {
			g_regs[i] = g_regs[--g_nregs];
			return;
		}
	}
}

void
rome_x_pump(void)
{
	if (g_dpy == NULL)
		return;
	while (XPending(g_dpy) > 0) {
		XEvent ev;
		XNextEvent(g_dpy, &ev);
		Window w;
		int x, y, wd, ht;
		if (ev.type == Expose) {
			w = ev.xexpose.window;
			x = ev.xexpose.x; y = ev.xexpose.y;
			wd = ev.xexpose.width; ht = ev.xexpose.height;
		} else if (ev.type == GraphicsExpose) {
			w = ev.xgraphicsexpose.drawable;
			x = ev.xgraphicsexpose.x; y = ev.xgraphicsexpose.y;
			wd = ev.xgraphicsexpose.width; ht = ev.xgraphicsexpose.height;
		} else {
			continue;
		}
		for (int i = 0; i < g_nregs; i++) {
			if (g_regs[i].win == w) {
				g_regs[i].fn(g_regs[i].owner, x, y, wd, ht);
				break;
			}
		}
	}
}

void
rome_render_move(RomeRenderer *r, int x, int y)
{
	if (g_dpy != NULL && r->win != 0) {
		XMoveWindow(g_dpy, (Window)r->win, x, y);
		XFlush(g_dpy);
	}
}

unsigned long
rome_render_window(RomeRenderer *r)
{
	return r->win;
}
