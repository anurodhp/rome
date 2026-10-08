/*
 * rome: the CPU renderer.
 *
 * Cells are drawn by the CPU into a client-side 32-bit image of the whole
 * window (an MIT-SHM segment when the server and the SysV limits allow it),
 * blending each glyph's atlas coverage between the cell's background and
 * foreground. Only the changed cells are drawn, and only their rectangles
 * are put on the window (XShmPutImage, else XPutImage). The scroll() entry
 * (XCopyArea inside the X server plus a memmove of the image) is currently
 * unused: the core redraws the rows that differ.
 */
#include "RomeRender.h"
#include "RomeX.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	RomeRenderer base;
	Display *dpy;
	Window win;
	GC gc;
	Visual *visual;
	int depth;
	XImage *img;
	XShmSegmentInfo shm;
	int use_shm;            /* -1 unknown, 0 no, 1 yes */
	uint32_t *pix;
	int stride;             /* in pixels */
} X11R;

static int g_shm_error;
static int
shm_error_handler(Display *d, XErrorEvent *e)
{
	(void)d;
	(void)e;
	g_shm_error = 1;
	return 0;
}

static void
image_free(X11R *x)
{
	if (x->img == NULL)
		return;
	if (x->shm.shmaddr != NULL) {
		XShmDetach(x->dpy, &x->shm);
		XSync(x->dpy, False);
		x->img->data = NULL;
		XDestroyImage(x->img);
		shmdt(x->shm.shmaddr);
		memset(&x->shm, 0, sizeof(x->shm));
	} else {
		XDestroyImage(x->img);      /* frees data */
	}
	x->img = NULL;
	x->pix = NULL;
}

/* The MIT-SHM image, or NULL (then the caller makes a plain one). Same
 * sequence as tools/userland_staging/mesa_xlib_vc4/xlib_vc4.c's shm_image():
 * IPC_RMID only after the server has attached, because xnu refuses to find a
 * segment already marked for removal (bsd/kern/sysv_shm.c,
 * shm_find_segment_by_shmid). */
static XImage *
image_shm(X11R *x, int w, int h)
{
	if (x->use_shm < 0)
		x->use_shm = getenv("ROME_NOSHM") == NULL && XShmQueryExtension(x->dpy);
	if (!x->use_shm)
		return NULL;
	XImage *img = XShmCreateImage(x->dpy, x->visual, x->depth, ZPixmap, NULL, &x->shm, w, h);
	if (img == NULL)
		return NULL;
	x->shm.shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * h, IPC_CREAT | 0600);
	if (x->shm.shmid < 0) {
		/* over shmmax (xnu default 4 MiB): this size uses XPutImage */
		XDestroyImage(img);
		memset(&x->shm, 0, sizeof(x->shm));
		return NULL;
	}
	x->shm.shmaddr = img->data = shmat(x->shm.shmid, NULL, 0);
	if (x->shm.shmaddr == (char *)-1) {
		shmctl(x->shm.shmid, IPC_RMID, NULL);
		img->data = NULL;
		XDestroyImage(img);
		memset(&x->shm, 0, sizeof(x->shm));
		return NULL;
	}
	x->shm.readOnly = True;
	g_shm_error = 0;
	XSync(x->dpy, False);
	XErrorHandler old = XSetErrorHandler(shm_error_handler);
	XShmAttach(x->dpy, &x->shm);
	XSync(x->dpy, False);
	XSetErrorHandler(old);
	shmctl(x->shm.shmid, IPC_RMID, NULL);
	if (g_shm_error) {
		img->data = NULL;
		XDestroyImage(img);
		shmdt(x->shm.shmaddr);
		memset(&x->shm, 0, sizeof(x->shm));
		x->use_shm = 0;
		return NULL;
	}
	return img;
}

static int
x11_resize(RomeRenderer *r, int w, int h)
{
	X11R *x = (X11R *)r;
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (x->img != NULL && w == r->width && h == r->height)
		return 1;
	image_free(x);
	XResizeWindow(x->dpy, x->win, w, h);
	XImage *img = image_shm(x, w, h);
	if (img == NULL) {
		img = XCreateImage(x->dpy, x->visual, x->depth, ZPixmap, 0, NULL, w, h, 32, 0);
		if (img == NULL)
			return 0;
		img->data = malloc((size_t)img->bytes_per_line * h);
		if (img->data == NULL) {
			XDestroyImage(img);
			return 0;
		}
	}
	if (img->bits_per_pixel != 32) {
		fprintf(stderr, "rome: the X11 renderer needs a 32 bpp visual (got %d)\n", img->bits_per_pixel);
		x->img = img;
		image_free(x);
		return 0;
	}
	x->img = img;
	x->pix = (uint32_t *)img->data;
	x->stride = img->bytes_per_line / 4;
	r->width = w;
	r->height = h;
	return 1;
}

static void
x11_begin(RomeRenderer *r)
{
	(void)r;
}

static void
fill(X11R *x, int x0, int y0, int w, int h, uint32_t c)
{
	if (x0 < 0) { w += x0; x0 = 0; }
	if (y0 < 0) { h += y0; y0 = 0; }
	if (x0 + w > x->base.width) w = x->base.width - x0;
	if (y0 + h > x->base.height) h = x->base.height - y0;
	if (w <= 0 || h <= 0)
		return;
	for (int y = y0; y < y0 + h; y++) {
		uint32_t *p = x->pix + (size_t)y * x->stride + x0;
		for (int i = 0; i < w; i++)
			p[i] = c;
	}
}

static void
x11_clear(RomeRenderer *r, int cols, int rows)
{
	X11R *x = (X11R *)r;
	RomeFont *f = r->font;
	int gw = cols * f->cell_w, gh = rows * f->cell_h;
	fill(x, 0, 0, r->width, r->pady, r->padbg);
	fill(x, 0, r->pady + gh, r->width, r->height - r->pady - gh, r->padbg);
	fill(x, 0, r->pady, r->padx, gh, r->padbg);
	fill(x, r->padx + gw, r->pady, r->width - r->padx - gw, gh, r->padbg);
}

/* out = bg + (fg - bg) * a / 255, per channel, exact for a = 0 and 255 */
static inline uint32_t
blend(uint32_t bg, uint32_t fg, unsigned a)
{
	unsigned na = 255 - a;
	unsigned rb = (bg & 0xff00ff) * na + (fg & 0xff00ff) * a + 0x800080;
	unsigned g = (bg & 0x00ff00) * na + (fg & 0x00ff00) * a + 0x008000;
	rb = ((rb + ((rb >> 8) & 0xff00ff)) >> 8) & 0xff00ff;
	g = ((g + ((g >> 8) & 0x00ff00)) >> 8) & 0x00ff00;
	return rb | g;
}

static void
draw_cell(X11R *x, int px, int py, int ncells, const RomeCell *c)
{
	RomeFont *f = x->base.font;
	int cw = f->cell_w * ncells, ch = f->cell_h;
	uint32_t fg = c->fg, bg = c->bg;
	/* clip to the image (a window smaller than the grid while resizing) */
	if (px + cw > x->base.width || py + ch > x->base.height || px < 0 || py < 0) {
		if (px >= x->base.width || py >= x->base.height)
			return;
		if (px + cw > x->base.width) cw = x->base.width - px;
		if (py + ch > x->base.height) ch = x->base.height - py;
	}
	int style = (c->attrs & ROME_ATTR_BOLD ? ROME_STYLE_BOLD : 0) | (c->attrs & ROME_ATTR_ITALIC ? ROME_STYLE_ITALIC : 0);
	int slot = rome_font_glyph(f, c->ch, style, ncells == 2);
	if (slot < 0) {
		fill(x, px, py, cw, ch, bg);
	} else {
		const uint8_t *a = f->atlas + (size_t)rome_font_slot_y(f, slot) * f->atlas_w + rome_font_slot_x(f, slot);
		for (int y = 0; y < ch; y++, a += f->atlas_w) {
			uint32_t *p = x->pix + (size_t)(py + y) * x->stride + px;
			for (int i = 0; i < cw; i++) {
				unsigned v = a[i];
				p[i] = v == 0 ? bg : v == 255 ? fg : blend(bg, fg, v);
			}
		}
	}
	for (int k = 0; k < 2; k++) {
		if (c->mark[k] == 0)
			continue;
		int ms = rome_font_glyph(f, c->mark[k], style | ROME_STYLE_MARK, ncells == 2);
		if (ms < 0)
			continue;
		const uint8_t *a = f->atlas + (size_t)rome_font_slot_y(f, ms) * f->atlas_w + rome_font_slot_x(f, ms);
		for (int y = 0; y < ch; y++, a += f->atlas_w) {
			uint32_t *p = x->pix + (size_t)(py + y) * x->stride + px;
			for (int i = 0; i < cw; i++)
				if (a[i] != 0)
					p[i] = blend(p[i], fg, a[i]);
		}
	}
	int lp = f->line_px;
	if (c->attrs & ROME_ATTR_UNDERLINE)
		fill(x, px, py + f->underline_y, cw, lp, fg);
	if (c->attrs & ROME_ATTR_STRIKE)
		fill(x, px, py + f->ascent - f->ascent / 3, cw, lp, fg);
	if (c->attrs & ROME_ATTR_CUR_UNDER)
		fill(x, px, py + ch - 2 * lp, cw, 2 * lp, fg);
	if (c->attrs & ROME_ATTR_CUR_BAR)
		fill(x, px, py, 2 * lp, ch, fg);
	if (c->attrs & ROME_ATTR_CUR_BOX) {
		fill(x, px, py, cw, lp, fg);
		fill(x, px, py + ch - lp, cw, lp, fg);
		fill(x, px, py, lp, ch, fg);
		fill(x, px + cw - lp, py, lp, ch, fg);
	}
}

static void
x11_draw_row(RomeRenderer *r, int row, int x0, int x1, const RomeCell *cells)
{
	X11R *x = (X11R *)r;
	RomeFont *f = r->font;
	double t0 = rome_now_ms();
	int py = r->pady + row * f->cell_h;
	for (int i = x0; i < x1; i++) {
		const RomeCell *c = &cells[i];
		if (c->width == 0)
			continue;       /* right half of a wide cell: drawn with its left half */
		int n = c->width == 2 ? 2 : 1;
		draw_cell(x, r->padx + i * f->cell_w, py, n, c);
	}
	r->t_draw += rome_now_ms() - t0;
}

static int
x11_scroll(RomeRenderer *r, int top, int bottom, int dy)
{
	X11R *x = (X11R *)r;
	RomeFont *f = r->font;
	int sy = r->pady + top * f->cell_h, h = (bottom - top) * f->cell_h, dyp = dy * f->cell_h;
	if (h <= 0 || sy + dyp < 0 || sy + h + dyp > r->height)
		return 0;
	double t0 = rome_now_ms();
	memmove(x->pix + (size_t)(sy + dyp) * x->stride, x->pix + (size_t)sy * x->stride,
	    (size_t)h * x->stride * 4);
	XCopyArea(x->dpy, x->win, x->win, x->gc, 0, sy, r->width, h, 0, sy + dyp);
	r->t_present += rome_now_ms() - t0;
	return 1;
}

static void
x11_present(RomeRenderer *r, const RomeRect *rects, int n)
{
	X11R *x = (X11R *)r;
	double t0 = rome_now_ms();
	for (int i = 0; i < n; i++) {
		RomeRect b = rects[i];
		if (b.x < 0) { b.w += b.x; b.x = 0; }
		if (b.y < 0) { b.h += b.y; b.y = 0; }
		if (b.x + b.w > r->width) b.w = r->width - b.x;
		if (b.y + b.h > r->height) b.h = r->height - b.y;
		if (b.w <= 0 || b.h <= 0)
			continue;
		if (x->shm.shmaddr != NULL)
			XShmPutImage(x->dpy, x->win, x->gc, x->img, b.x, b.y, b.x, b.y, b.w, b.h, False);
		else
			XPutImage(x->dpy, x->win, x->gc, x->img, b.x, b.y, b.x, b.y, b.w, b.h);
		r->px_present += (unsigned long)b.w * b.h;
	}
	/* The server must have read the segment before the next frame writes
	 * it; a round trip also bounds how far ahead of the server we run. */
	XSync(x->dpy, False);
	r->t_present += rome_now_ms() - t0;
}

static void
x11_destroy(RomeRenderer *r)
{
	X11R *x = (X11R *)r;
	image_free(x);
	if (x->gc != NULL)
		XFreeGC(x->dpy, x->gc);
	if (x->win != 0) {
		rome_x_unregister(x->win);
		XDestroyWindow(x->dpy, x->win);
		XSync(x->dpy, False);
	}
	free(x);
}

RomeRenderer *
rome_render_x11_new(RomeFont *font, unsigned long parent, int px, int py, int w, int h)
{
	Display *dpy = rome_x_display();
	if (dpy == NULL)
		return NULL;
	int scr = DefaultScreen(dpy);
	Visual *vis = DefaultVisual(dpy, scr);
	if (vis->class != TrueColor || vis->red_mask != 0xff0000 || vis->green_mask != 0xff00 || vis->blue_mask != 0xff) {
		fprintf(stderr, "rome: the X11 renderer needs a 24-bit TrueColor default visual\n");
		return NULL;
	}
	X11R *x = calloc(1, sizeof(*x));
	if (x == NULL)
		return NULL;
	x->dpy = dpy;
	x->visual = vis;
	x->depth = DefaultDepth(dpy, scr);
	x->use_shm = -1;
	XSetWindowAttributes wa;
	memset(&wa, 0, sizeof(wa));
	wa.background_pixmap = None;
	wa.border_pixel = 0;
	wa.colormap = DefaultColormap(dpy, scr);
	wa.event_mask = ExposureMask;
	wa.bit_gravity = NorthWestGravity;
	x->win = XCreateWindow(dpy, (Window)parent, px, py, w > 0 ? w : 1, h > 0 ? h : 1, 0, x->depth,
	    InputOutput, vis, CWBackPixmap | CWBorderPixel | CWColormap | CWEventMask | CWBitGravity, &wa);
	x->gc = XCreateGC(dpy, x->win, 0, NULL);
	XSetGraphicsExposures(dpy, x->gc, True);
	XMapWindow(dpy, x->win);

	RomeRenderer *r = &x->base;
	r->name = "x11";
	r->font = font;
	r->win = x->win;
	r->resize = x11_resize;
	r->begin = x11_begin;
	r->clear = x11_clear;
	r->draw_row = x11_draw_row;
	r->scroll = x11_scroll;
	r->present = x11_present;
	r->destroy = x11_destroy;
	if (!x11_resize(r, w, h)) {
		x11_destroy(r);
		return NULL;
	}
	return r;
}
