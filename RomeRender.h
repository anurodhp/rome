/*
 * rome: the cell model shared by the terminal core and the renderers.
 *
 * The core (RomeTerm.c) turns libghostty-vt's screen and scrollback into rows
 * of RomeCell, already resolved to RGB with reverse video, selection and the
 * cursor applied. A renderer only has to put cells on the screen; it never
 * sees libghostty-vt.
 *
 * Both renderers draw into a child X window of the GNUstep window, on rome's
 * own X connection (RomeX.c), and keep the window's pixels in step with a
 * retained image of the whole grid (a client-side MIT-SHM image for the X11
 * renderer, the GL back buffer for the GL renderer). Each frame draws only
 * the cells that changed and puts only those pixels on the screen; a scroll
 * is an XCopyArea inside the X server.
 */
#ifndef ROME_RENDER_H
#define ROME_RENDER_H

#include <stdint.h>
#include "RomeFont.h"

enum {
	ROME_ATTR_BOLD      = 1 << 0,
	ROME_ATTR_ITALIC    = 1 << 1,
	ROME_ATTR_UNDERLINE = 1 << 2,
	ROME_ATTR_STRIKE    = 1 << 3,
	/* cursor decorations, drawn in the cell's foreground colour */
	ROME_ATTR_CUR_BOX   = 1 << 4,   /* hollow box: window not key */
	ROME_ATTR_CUR_UNDER = 1 << 5,   /* underline cursor shape */
	ROME_ATTR_CUR_BAR   = 1 << 6,   /* bar cursor shape */
};

typedef struct {
	uint32_t ch;        /* Unicode code point; 0 or ' ' draws nothing */
	uint32_t fg, bg;    /* 0x00RRGGBB */
	uint8_t attrs;      /* ROME_ATTR_* */
	uint8_t width;      /* 1, 2 (wide), 0 (right half of a wide cell) */
	uint16_t pad;
} RomeCell;

typedef struct {
	int x, y, w, h;     /* pixels, origin top left */
} RomeRect;

typedef struct RomeRenderer RomeRenderer;
struct RomeRenderer {
	const char *name;
	RomeFont *font;
	unsigned long win;          /* the child X window (RomeX.c's connection) */
	int width, height;          /* window size in pixels */
	int padx, pady;             /* grid origin in the window */
	uint32_t padbg;             /* colour of the margin around the grid */

	/* Resize the window and its retained image; the caller redraws every
	 * row next. Returns 0 on failure. */
	int (*resize)(RomeRenderer *r, int width, int height);
	/* Start a frame (make the GL context current, upload new glyphs). */
	void (*begin)(RomeRenderer *r);
	/* Fill the margin around a cols x rows grid (after a resize). */
	void (*clear)(RomeRenderer *r, int cols, int rows);
	/* Draw cells [x0, x1) of grid row `row` into the retained image. `cells`
	 * is the whole row. */
	void (*draw_row)(RomeRenderer *r, int row, int x0, int x1, const RomeCell *cells);
	/* Rows [top, bottom) moved by dy rows (negative = up), full width.
	 * Returns 0 when not done (the caller then redraws the destination),
	 * 1 when both the window and the retained image moved, 2 when the window
	 * moved but the retained image did not: the caller then redraws the
	 * destination rows without putting them on the screen. */
	int (*scroll)(RomeRenderer *r, int top, int bottom, int dy);
	/* Put rectangles of the retained image on the screen and finish the
	 * frame. */
	void (*present)(RomeRenderer *r, const RomeRect *rects, int n);
	void (*destroy)(RomeRenderer *r);

	/* Set by a renderer whose retained image went stale (the GL renderer
	 * when the glyph atlas was reset mid-frame); the core then redraws and
	 * puts everything on the next frame. */
	int want_full_redraw;

	/* ROME_STATS: time spent in draw (CPU or GL calls) and in present */
	double t_draw, t_present;
	unsigned long px_present;   /* pixels put on the screen */
};

/* Implementations. `parent` is the X window of the GNUstep window; x, y is
 * the view's origin in it. Each returns NULL when it cannot run (no GLX
 * visual, ...). */
RomeRenderer *rome_render_x11_new(RomeFont *font, unsigned long parent, int x, int y, int w, int h);
RomeRenderer *rome_render_gl_new(RomeFont *font, unsigned long parent, int x, int y, int w, int h);

/* Map or unmap the renderer's child window (a hidden tab). */
void rome_render_show(RomeRenderer *r, int show);

/* Move the renderer's child window inside its parent. */
void rome_render_move(RomeRenderer *r, int x, int y);
unsigned long rome_render_window(RomeRenderer *r);

double rome_now_ms(void);

#endif
