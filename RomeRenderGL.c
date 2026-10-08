/*
 * rome: the OpenGL renderer (GL 2.1 / GLES 2.0 class: what Mesa's vc4
 * driver gives on the Pi's VideoCore IV, and softpipe elsewhere).
 *
 * The glyph atlas is one GL_ALPHA texture; changed atlas rows are uploaded
 * with glTexSubImage2D just before they are drawn. Each changed cell is two
 * quads, its background (from the atlas' solid slot) and its glyph, batched
 * into two vertex arrays and drawn with one glDrawElements each (backgrounds
 * first). Fixed-function texturing, GL_MODULATE: the quad's colour times the
 * texture's alpha, blended over the background.
 *
 * The back buffer is never swapped, so it always holds the whole grid and a
 * frame draws only the cells that changed. It is put on the screen with
 * glXCopySubBufferMESA (GLX_MESA_copy_sub_buffer, Mesa's xlib GLX:
 * src/gallium/frontends/glx/xlib/glx_api.c), which presents one box of the
 * back buffer: the bounding box of the changed cells. On the Pi that box is
 * what the X server copies (tools/userland_staging/mesa_xlib_vc4/xlib_vc4.c,
 * put_boxes()), though the driver still reads the whole frame back from the
 * GPU (v3d_drm_read_bo of stride x height), which is the floor of a GL
 * frame's cost.
 *
 * The scroll() entry (an XCopyArea inside the X server, the moved rows then
 * redrawn into the back buffer but not presented) is currently unused: the
 * core redraws the rows that differ.
 */
#include "RomeRender.h"
#include "RomeX.h"

#include <GL/gl.h>
#include <GL/glx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_QUADS 16383     /* 4 vertices each, GLushort indices */

typedef struct {
	GLshort x, y;
	GLshort u, v;
	GLubyte c[4];
} Vtx;

typedef struct {
	Vtx *v;
	int n, cap;             /* quads */
} Batch;

typedef void (*CopySubFn)(Display *, GLXDrawable, int, int, int, int);

typedef struct {
	RomeRenderer base;
	Display *dpy;
	Window win;
	GC gc;
	GLXContext ctx;
	GLuint tex;
	unsigned tex_gen;
	int tex_ok;
	Batch bg, fg;
	GLushort *idx;
	CopySubFn copysub;
	int sized;              /* the GL buffer has been validated at width x height */
	Colormap cmap;
} GLR;

/* A quad of `rgb`. u, v is the atlas position of the glyph; a solid quad
 * (u < 0) samples one texel inside the atlas' solid slot 0 for all of its
 * corners, whatever its size: a background run wider than the slot would
 * otherwise read the glyphs next to it. */
static void
batch_quad(Batch *b, int x, int y, int w, int h, int u, int v, uint32_t rgb)
{
	if (b->n == b->cap) {
		b->cap = b->cap ? 2 * b->cap : 1024;
		b->v = realloc(b->v, (size_t)b->cap * 4 * sizeof(Vtx));
	}
	Vtx *q = b->v + (size_t)b->n * 4;
	GLubyte r = rgb >> 16, g = rgb >> 8, bl = rgb;
	int u1, v1, uw, vh;
	if (u < 0) {
		u = u1 = 1;
		v = v1 = 1;
		uw = vh = 0;
	} else {
		u1 = u + w;
		v1 = v + h;
		uw = w;
		vh = h;
	}
	(void)uw; (void)vh;
	q[0] = (Vtx){ x, y, u, v, { r, g, bl, 255 } };
	q[1] = (Vtx){ x + w, y, u1, v, { r, g, bl, 255 } };
	q[2] = (Vtx){ x + w, y + h, u1, v1, { r, g, bl, 255 } };
	q[3] = (Vtx){ x, y + h, u, v1, { r, g, bl, 255 } };
	b->n++;
}

static void
upload_atlas(GLR *g)
{
	RomeFont *f = g->base.font;
	glBindTexture(GL_TEXTURE_2D, g->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	if (!g->tex_ok || g->tex_gen != f->generation) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, f->atlas_w, f->atlas_h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, f->atlas);
		g->tex_ok = 1;
		g->tex_gen = f->generation;
	} else if (f->dirty_y1 > f->dirty_y0) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, f->dirty_y0, f->atlas_w, f->dirty_y1 - f->dirty_y0,
		    GL_ALPHA, GL_UNSIGNED_BYTE, f->atlas + (size_t)f->dirty_y0 * f->atlas_w);
	}
	f->dirty_y0 = f->dirty_y1 = 0;
}

static void
draw_batch(GLR *g, Batch *b)
{
	for (int done = 0; done < b->n; done += MAX_QUADS) {
		int n = b->n - done < MAX_QUADS ? b->n - done : MAX_QUADS;
		Vtx *v = b->v + (size_t)done * 4;
		glVertexPointer(2, GL_SHORT, sizeof(Vtx), &v->x);
		glTexCoordPointer(2, GL_SHORT, sizeof(Vtx), &v->u);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vtx), v->c);
		glDrawElements(GL_TRIANGLES, n * 6, GL_UNSIGNED_SHORT, g->idx);
	}
	b->n = 0;
}

static void
flush_quads(GLR *g)
{
	if (g->bg.n == 0 && g->fg.n == 0)
		return;
	upload_atlas(g);
	draw_batch(g, &g->bg);
	draw_batch(g, &g->fg);
}

static void
gl_begin(RomeRenderer *r)
{
	GLR *g = (GLR *)r;
	double t0 = rome_now_ms();
	glXMakeCurrent(g->dpy, g->win, g->ctx);
	RomeFont *f = r->font;
	glViewport(0, 0, r->width, r->height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, r->width, r->height, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glScalef(1.0f / f->atlas_w, 1.0f / f->atlas_h, 1.0f);
	g->sized = 1;
	r->t_draw += rome_now_ms() - t0;
}

static int
gl_resize(RomeRenderer *r, int w, int h)
{
	GLR *g = (GLR *)r;
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (w == r->width && h == r->height)
		return 1;
	XResizeWindow(g->dpy, g->win, w, h);
	r->width = w;
	r->height = h;
	/* Mesa's xlib GLX picks the new size up with XGetGeometry when the
	 * buffer is next validated or made current (xm_api.c,
	 * xmesa_check_buffer_size); the server must see the resize first. */
	XSync(g->dpy, False);
	glXMakeCurrent(g->dpy, None, NULL);
	return 1;
}

static void
gl_clear(RomeRenderer *r, int cols, int rows)
{
	GLR *g = (GLR *)r;
	(void)cols;
	(void)rows;
	uint32_t c = r->padbg;
	glClearColor(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	(void)g;
}

static void
gl_draw_row(RomeRenderer *r, int row, int x0, int x1, const RomeCell *cells)
{
	GLR *g = (GLR *)r;
	RomeFont *f = r->font;
	double t0 = rome_now_ms();
	int cw = f->cell_w, ch = f->cell_h, lp = f->line_px;
	int py = r->pady + row * ch;
	unsigned gen = f->generation;
	/* backgrounds: one quad per run of equal colour */
	for (int i = x0; i < x1;) {
		int j = i + 1;
		while (j < x1 && cells[j].bg == cells[i].bg)
			j++;
		batch_quad(&g->bg, r->padx + i * cw, py, (j - i) * cw, ch, -1, -1, cells[i].bg);
		i = j;
	}
	for (int i = x0; i < x1; i++) {
		const RomeCell *c = &cells[i];
		if (c->width == 0)
			continue;
		int n = c->width == 2 ? 2 : 1;
		int px = r->padx + i * cw, w = n * cw;
		int style = (c->attrs & ROME_ATTR_BOLD ? ROME_STYLE_BOLD : 0) | (c->attrs & ROME_ATTR_ITALIC ? ROME_STYLE_ITALIC : 0);
		int slot = rome_font_glyph(f, c->ch, style, n == 2);
		if (slot >= 0)
			batch_quad(&g->fg, px, py, w, ch, rome_font_slot_x(f, slot), rome_font_slot_y(f, slot), c->fg);
		for (int k = 0; k < 2; k++) {
			if (c->mark[k] == 0)
				continue;
			int ms = rome_font_glyph(f, c->mark[k], style | ROME_STYLE_MARK, n == 2);
			if (ms >= 0)
				batch_quad(&g->fg, px, py, w, ch, rome_font_slot_x(f, ms), rome_font_slot_y(f, ms), c->fg);
		}
		if (c->attrs & (ROME_ATTR_UNDERLINE | ROME_ATTR_STRIKE | ROME_ATTR_CUR_UNDER | ROME_ATTR_CUR_BAR | ROME_ATTR_CUR_BOX)) {
			if (c->attrs & ROME_ATTR_UNDERLINE)
				batch_quad(&g->fg, px, py + f->underline_y, w, lp, -1, -1, c->fg);
			if (c->attrs & ROME_ATTR_STRIKE)
				batch_quad(&g->fg, px, py + f->ascent - f->ascent / 3, w, lp, -1, -1, c->fg);
			if (c->attrs & ROME_ATTR_CUR_UNDER)
				batch_quad(&g->fg, px, py + ch - 2 * lp, w, 2 * lp, -1, -1, c->fg);
			if (c->attrs & ROME_ATTR_CUR_BAR)
				batch_quad(&g->fg, px, py, 2 * lp, ch, -1, -1, c->fg);
			if (c->attrs & ROME_ATTR_CUR_BOX) {
				batch_quad(&g->fg, px, py, w, lp, -1, -1, c->fg);
				batch_quad(&g->fg, px, py + ch - lp, w, lp, -1, -1, c->fg);
				batch_quad(&g->fg, px, py, lp, ch, -1, -1, c->fg);
				batch_quad(&g->fg, px + w - lp, py, lp, ch, -1, -1, c->fg);
			}
		}
	}
	/* The atlas was reset while queueing: quads queued earlier name slots
	 * that now hold other glyphs. Draw what we have; the core redraws
	 * everything next frame. */
	if (f->generation != gen)
		r->want_full_redraw = 1;
	r->t_draw += rome_now_ms() - t0;
}

static int
gl_scroll(RomeRenderer *r, int top, int bottom, int dy)
{
	GLR *g = (GLR *)r;
	RomeFont *f = r->font;
	if (g->copysub == NULL)
		return 0;
	int sy = r->pady + top * f->cell_h, h = (bottom - top) * f->cell_h, dyp = dy * f->cell_h;
	if (h <= 0 || sy + dyp < 0 || sy + h + dyp > r->height)
		return 0;
	XCopyArea(g->dpy, g->win, g->win, g->gc, 0, sy, r->width, h, 0, sy + dyp);
	return 2;
}

static void
gl_present(RomeRenderer *r, const RomeRect *rects, int n)
{
	GLR *g = (GLR *)r;
	double t0 = rome_now_ms();
	flush_quads(g);
	double t1 = rome_now_ms();
	r->t_draw += t1 - t0;
	if (n == 0) {
		glFlush();
		return;
	}
	int x0 = r->width, y0 = r->height, x1 = 0, y1 = 0;
	for (int i = 0; i < n; i++) {
		if (rects[i].x < x0) x0 = rects[i].x;
		if (rects[i].y < y0) y0 = rects[i].y;
		if (rects[i].x + rects[i].w > x1) x1 = rects[i].x + rects[i].w;
		if (rects[i].y + rects[i].h > y1) y1 = rects[i].y + rects[i].h;
	}
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > r->width) x1 = r->width;
	if (y1 > r->height) y1 = r->height;
	if (x1 <= x0 || y1 <= y0) {
		glFlush();
		return;
	}
	if (g->copysub != NULL) {
		/* GLX's y axis points up */
		g->copysub(g->dpy, g->win, x0, r->height - y1, x1 - x0, y1 - y0);
		r->px_present += (unsigned long)(x1 - x0) * (y1 - y0);
	} else {
		glXSwapBuffers(g->dpy, g->win);
		r->px_present += (unsigned long)r->width * r->height;
		r->want_full_redraw = 1;    /* the back buffer is undefined after a swap */
	}
	r->t_present += rome_now_ms() - t1;
}

static void
gl_destroy(RomeRenderer *r)
{
	GLR *g = (GLR *)r;
	if (g->ctx != NULL) {
		glXMakeCurrent(g->dpy, None, NULL);
		glXDestroyContext(g->dpy, g->ctx);
	}
	if (g->gc != NULL)
		XFreeGC(g->dpy, g->gc);
	if (g->win != 0) {
		rome_x_unregister(g->win);
		XDestroyWindow(g->dpy, g->win);
	}
	if (g->cmap != 0)
		XFreeColormap(g->dpy, g->cmap);
	XSync(g->dpy, False);
	free(g->bg.v);
	free(g->fg.v);
	free(g->idx);
	free(g);
}

RomeRenderer *
rome_render_gl_new(RomeFont *font, unsigned long parent, int px, int py, int w, int h)
{
	Display *dpy = rome_x_display();
	if (dpy == NULL)
		return NULL;
	int scr = DefaultScreen(dpy);
	int attrs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
	XVisualInfo *vi = glXChooseVisual(dpy, scr, attrs);
	if (vi == NULL) {
		fprintf(stderr, "rome: no double-buffered RGB GLX visual\n");
		return NULL;
	}
	GLR *g = calloc(1, sizeof(*g));
	if (g == NULL) {
		XFree(vi);
		return NULL;
	}
	g->dpy = dpy;
	XSetWindowAttributes wa;
	memset(&wa, 0, sizeof(wa));
	wa.background_pixmap = None;
	wa.border_pixel = 0;
	g->cmap = wa.colormap = XCreateColormap(dpy, RootWindow(dpy, scr), vi->visual, AllocNone);
	wa.event_mask = ExposureMask;
	wa.bit_gravity = NorthWestGravity;
	g->win = XCreateWindow(dpy, (Window)parent, px, py, w > 0 ? w : 1, h > 0 ? h : 1, 0, vi->depth,
	    InputOutput, vi->visual, CWBackPixmap | CWBorderPixel | CWColormap | CWEventMask | CWBitGravity, &wa);
	g->gc = XCreateGC(dpy, g->win, 0, NULL);
	XSetGraphicsExposures(dpy, g->gc, True);
	XMapWindow(dpy, g->win);
	g->ctx = glXCreateContext(dpy, vi, NULL, True);
	XFree(vi);
	RomeRenderer *r = &g->base;
	r->name = "gl";
	r->font = font;
	r->win = g->win;
	r->resize = gl_resize;
	r->begin = gl_begin;
	r->clear = gl_clear;
	r->draw_row = gl_draw_row;
	r->scroll = gl_scroll;
	r->present = gl_present;
	r->destroy = gl_destroy;
	if (g->ctx == NULL || !glXMakeCurrent(dpy, g->win, g->ctx)) {
		fprintf(stderr, "rome: cannot create or bind a GLX context\n");
		gl_destroy(r);
		return NULL;
	}
	r->width = w > 0 ? w : 1;
	r->height = h > 0 ? h : 1;
	if (getenv("ROME_GL_NOCOPYSUB") == NULL) {
		const char *ext = glXQueryExtensionsString(dpy, scr);
		if (ext != NULL && strstr(ext, "GLX_MESA_copy_sub_buffer") != NULL)
			g->copysub = (CopySubFn)glXGetProcAddress((const GLubyte *)"glXCopySubBufferMESA");
	}
	if (getenv("ROME_STATS") != NULL)
		fprintf(stderr, "rome: GL_RENDERER %s, GL_VERSION %s, present %s\n", glGetString(GL_RENDERER),
		    glGetString(GL_VERSION), g->copysub ? "glXCopySubBufferMESA (changed box)" : "glXSwapBuffers (whole window)");
	g->idx = malloc(MAX_QUADS * 6 * sizeof(GLushort));
	for (int i = 0; i < MAX_QUADS; i++) {
		GLushort b = (GLushort)(i * 4);
		GLushort *p = g->idx + i * 6;
		p[0] = b; p[1] = b + 1; p[2] = b + 2;
		p[3] = b; p[4] = b + 2; p[5] = b + 3;
	}
	glGenTextures(1, &g->tex);
	glBindTexture(GL_TEXTURE_2D, g->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_DITHER);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	return r;
}
