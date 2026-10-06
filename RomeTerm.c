/*
 * rome: the terminal core (see RomeTerm.h).
 *
 * Damage: libvterm's screen runs with VTERM_DAMAGE_SCROLL, so it merges a
 * burst of output into one scroll (moverect) plus one damaged rectangle,
 * delivered by vterm_screen_flush_damage() at the start of a frame (or
 * earlier, when the scroll region changes). Cost per frame is therefore
 * bounded by the screen, not by how much output arrived.
 *
 * Per screen row the core keeps a span of columns to draw and put on the
 * screen, and a flag for rows the renderer must redraw without putting them
 * (the GL renderer after a server-side scroll). A full-width scroll is handed
 * to the renderer, which moves the window's pixels inside the X server; the
 * row spans move with it.
 */
#define _DARWIN_C_SOURCE
#include "RomeTerm.h"

#include <vterm.h>

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

typedef struct {
	int cols;
	RomeCell *cells;
} SbLine;

struct RomeTerm {
	VTerm *vt;
	VTermScreen *vs;
	VTermState *st;
	int rows, cols;
	int fd;
	pid_t pid;
	RomeTermCallbacks cb;
	void *owner;
	RomeTheme theme;
	RomeRenderer *r;

	/* damage */
	int *dx0, *dx1;         /* columns [dx0, dx1) to draw and present */
	uint8_t *regen;         /* redraw the whole row, do not present */
	int full;               /* clear + redraw + present everything */
	int has_expose;
	int ex0, ey0, ex1, ey1;

	/* cursor */
	VTermPos cur;
	int cur_visible, cur_blink, cur_shape;
	int focused, phase;
	int drawn_row, drawn_col, drawn_kind;   /* what the screen shows; row -1 = none */

	/* scrollback ring */
	SbLine *sb;
	int sb_cap, sb_head, sb_count;
	long long sb_pushed;    /* lines ever pushed: absolute line of screen row 0 */
	int view;               /* lines scrolled back */

	/* selection, absolute lines, [start, end) in reading order */
	int has_sel;
	long long sl0, sl1;
	int sc0, sc1;

	int mouse, alt;
	char *title;
	size_t title_len;

	char *out;
	size_t out_len, out_cap;
	RomeCell *rowbuf;
	int rowbuf_cols;
};

/* ---- helpers ---- */

static uint32_t
color_rgb(RomeTerm *t, VTermColor c, int is_fg)
{
	if (VTERM_COLOR_IS_DEFAULT_FG(&c) && is_fg)
		return t->theme.fg;
	if (VTERM_COLOR_IS_DEFAULT_BG(&c) && !is_fg)
		return t->theme.bg;
	if (VTERM_COLOR_IS_DEFAULT_FG(&c))
		return t->theme.fg;
	if (VTERM_COLOR_IS_DEFAULT_BG(&c))
		return t->theme.bg;
	vterm_screen_convert_color_to_rgb(t->vs, &c);
	return ((uint32_t)c.rgb.red << 16) | ((uint32_t)c.rgb.green << 8) | c.rgb.blue;
}

static void
convert_cell(RomeTerm *t, const VTermScreenCell *vc, RomeCell *rc)
{
	uint32_t ch = vc->chars[0];
	rc->ch = ch == (uint32_t)-1 ? 0 : ch;
	rc->width = ch == (uint32_t)-1 ? 0 : (uint8_t)vc->width;
	uint32_t fg = color_rgb(t, vc->fg, 1), bg = color_rgb(t, vc->bg, 0);
	if (vc->attrs.reverse) {
		uint32_t x = fg;
		fg = bg;
		bg = x;
	}
	if (vc->attrs.conceal)
		fg = bg;
	rc->fg = fg;
	rc->bg = bg;
	rc->attrs = (vc->attrs.bold ? ROME_ATTR_BOLD : 0) | (vc->attrs.italic ? ROME_ATTR_ITALIC : 0) |
	    (vc->attrs.underline ? ROME_ATTR_UNDERLINE : 0) | (vc->attrs.strike ? ROME_ATTR_STRIKE : 0);
	rc->pad = 0;
}

static void
mark(RomeTerm *t, int row, int x0, int x1)
{
	if (row < 0 || row >= t->rows)
		return;
	if (x0 < 0) x0 = 0;
	if (x1 > t->cols) x1 = t->cols;
	if (x1 <= x0)
		return;
	if (t->dx1[row] <= t->dx0[row]) {
		t->dx0[row] = x0;
		t->dx1[row] = x1;
	} else {
		if (x0 < t->dx0[row]) t->dx0[row] = x0;
		if (x1 > t->dx1[row]) t->dx1[row] = x1;
	}
}

void
rome_term_damage_all(RomeTerm *t)
{
	t->full = 1;
}

static int
alloc_rows(RomeTerm *t, int rows, int cols)
{
	int *a = realloc(t->dx0, rows * sizeof(int)), *b;
	if (a == NULL)
		return 0;
	t->dx0 = a;
	if ((b = realloc(t->dx1, rows * sizeof(int))) == NULL)
		return 0;
	t->dx1 = b;
	uint8_t *g = realloc(t->regen, rows);
	if (g == NULL)
		return 0;
	t->regen = g;
	memset(t->dx0, 0, rows * sizeof(int));
	memset(t->dx1, 0, rows * sizeof(int));
	memset(t->regen, 0, rows);
	if (cols > t->rowbuf_cols) {
		RomeCell *rb = realloc(t->rowbuf, (cols + 1) * sizeof(RomeCell));
		if (rb == NULL)
			return 0;
		t->rowbuf = rb;
		t->rowbuf_cols = cols;
	}
	return 1;
}

/* ---- libvterm screen callbacks ---- */

static int
cb_damage(VTermRect rect, void *user)
{
	RomeTerm *t = user;
	if (t->view != 0) {
		t->full = 1;
		return 1;
	}
	/* one column of slack each side: the other half of a wide character */
	for (int row = rect.start_row; row < rect.end_row; row++)
		mark(t, row, rect.start_col - 1, rect.end_col + 1);
	return 1;
}

static int
cb_moverect(VTermRect dest, VTermRect src, void *user)
{
	RomeTerm *t = user;
	if (t->view != 0) {
		t->full = 1;
		return 1;
	}
	if (t->r == NULL || t->full || src.start_col != 0 || src.end_col != t->cols ||
	    dest.start_col != 0 || dest.end_col != t->cols)
		return 0;
	int dy = dest.start_row - src.start_row;
	int top = src.start_row, bottom = src.end_row;
	if (dy == 0)
		return 1;
	/* An exposed area not yet repainted would move with the pixels. */
	if (t->has_expose) {
		t->ex0 = 0; t->ey0 = 0;
		t->ex1 = t->r->width; t->ey1 = t->r->height;
	}
	int how = t->r->scroll(t->r, top, bottom, dy);
	if (how == 0)
		return 0;
	/* The pending spans move with the content. */
	int n = bottom - top;
	int *s0 = malloc(n * sizeof(int)), *s1 = malloc(n * sizeof(int));
	uint8_t *sg = malloc(n);
	if (s0 == NULL || s1 == NULL || sg == NULL) {
		free(s0); free(s1); free(sg);
		t->full = 1;
		return 1;
	}
	memcpy(s0, t->dx0 + top, n * sizeof(int));
	memcpy(s1, t->dx1 + top, n * sizeof(int));
	memcpy(sg, t->regen + top, n);
	for (int i = 0; i < n; i++) {
		int d = top + dy + i;
		t->dx0[d] = s0[i];
		t->dx1[d] = s1[i];
		t->regen[d] = sg[i] || how == 2;
	}
	free(s0); free(s1); free(sg);
	/* rows uncovered by the move: libvterm damages them; clear the stale
	 * spans that were there */
	if (t->drawn_row >= top && t->drawn_row < bottom)
		t->drawn_row += dy;
	else if (t->drawn_row >= top + dy && t->drawn_row < bottom + dy)
		t->drawn_row = -1;      /* scrolled over: the content there is redrawn */
	return 1;
}

static int
cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
	RomeTerm *t = user;
	(void)oldpos;
	t->cur = pos;
	t->cur_visible = visible;
	return 1;
}

static int
cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
	RomeTerm *t = user;
	switch (prop) {
	case VTERM_PROP_CURSORVISIBLE:
		t->cur_visible = val->boolean;
		break;
	case VTERM_PROP_CURSORBLINK:
		t->cur_blink = val->boolean;
		break;
	case VTERM_PROP_CURSORSHAPE:
		t->cur_shape = val->number;
		break;
	case VTERM_PROP_ALTSCREEN:
		t->alt = val->boolean;
		t->view = 0;
		t->full = 1;
		break;
	case VTERM_PROP_MOUSE:
		t->mouse = val->number;
		break;
	case VTERM_PROP_TITLE: {
		VTermStringFragment fr = val->string;
		if (fr.initial)
			t->title_len = 0;
		char *n = realloc(t->title, t->title_len + fr.len + 1);
		if (n == NULL)
			break;
		t->title = n;
		memcpy(t->title + t->title_len, fr.str, fr.len);
		t->title_len += fr.len;
		t->title[t->title_len] = 0;
		if (fr.final && t->cb.title != NULL)
			t->cb.title(t->owner, t->title);
		break;
	}
	default:
		break;
	}
	return 1;
}

static int
cb_bell(void *user)
{
	RomeTerm *t = user;
	if (t->cb.bell != NULL)
		t->cb.bell(t->owner);
	return 1;
}

static int
cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
	RomeTerm *t = user;
	if (t->sb_cap == 0)
		return 1;
	SbLine *l;
	if (t->sb_count == t->sb_cap) {
		l = &t->sb[t->sb_head];
		t->sb_head = (t->sb_head + 1) % t->sb_cap;
		t->sb_count--;
	} else {
		l = &t->sb[(t->sb_head + t->sb_count) % t->sb_cap];
	}
	/* trailing blanks with the default background are not stored */
	int n = cols;
	while (n > 0 && cells[n - 1].chars[0] == 0 && VTERM_COLOR_IS_DEFAULT_BG(&cells[n - 1].bg) &&
	    !cells[n - 1].attrs.reverse)
		n--;
	if (l->cells == NULL || l->cols < n) {
		RomeCell *c = realloc(l->cells, (n ? n : 1) * sizeof(RomeCell));
		if (c == NULL)
			return 1;
		l->cells = c;
	}
	l->cols = n;
	for (int i = 0; i < n; i++)
		convert_cell(t, &cells[i], &l->cells[i]);
	t->sb_count++;
	t->sb_pushed++;
	if (t->view != 0) {
		/* keep the view on the same lines */
		if (t->view < t->sb_count)
			t->view++;
		t->full = 1;
	}
	return 1;
}

static int
cb_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
	RomeTerm *t = user;
	if (t->sb_count == 0)
		return 0;
	SbLine *l = &t->sb[(t->sb_head + t->sb_count - 1) % t->sb_cap];
	VTermColor dfg, dbg;
	vterm_state_get_default_colors(t->st, &dfg, &dbg);
	for (int i = 0; i < cols; i++) {
		VTermScreenCell *c = &cells[i];
		memset(c, 0, sizeof(*c));
		c->width = 1;
		c->fg = dfg;
		c->bg = dbg;
		if (i < l->cols) {
			const RomeCell *rc = &l->cells[i];
			c->chars[0] = rc->width == 0 ? (uint32_t)-1 : rc->ch;
			c->width = rc->width ? rc->width : 1;
			c->attrs.bold = !!(rc->attrs & ROME_ATTR_BOLD);
			c->attrs.italic = !!(rc->attrs & ROME_ATTR_ITALIC);
			c->attrs.underline = !!(rc->attrs & ROME_ATTR_UNDERLINE);
			c->attrs.strike = !!(rc->attrs & ROME_ATTR_STRIKE);
			if (rc->fg != t->theme.fg)
				vterm_color_rgb(&c->fg, rc->fg >> 16, rc->fg >> 8, rc->fg);
			if (rc->bg != t->theme.bg)
				vterm_color_rgb(&c->bg, rc->bg >> 16, rc->bg >> 8, rc->bg);
		}
	}
	t->sb_count--;
	t->sb_pushed--;
	if (t->view > t->sb_count)
		t->view = t->sb_count;
	return 1;
}

static int
cb_sb_clear(void *user)
{
	RomeTerm *t = user;
	t->sb_count = 0;
	t->view = 0;
	t->full = 1;
	return 1;
}

static const VTermScreenCallbacks screen_cbs = {
	.damage = cb_damage,
	.moverect = cb_moverect,
	.movecursor = cb_movecursor,
	.settermprop = cb_settermprop,
	.bell = cb_bell,
	.sb_pushline = cb_sb_pushline,
	.sb_popline = cb_sb_popline,
	.sb_clear = cb_sb_clear,
};

/* ---- output to the pty ---- */

static void
cb_output(const char *s, size_t len, void *user)
{
	RomeTerm *t = user;
	if (t->out_len + len > t->out_cap) {
		size_t cap = t->out_cap ? t->out_cap : 4096;
		while (cap < t->out_len + len)
			cap *= 2;
		char *n = realloc(t->out, cap);
		if (n == NULL)
			return;
		t->out = n;
		t->out_cap = cap;
	}
	memcpy(t->out + t->out_len, s, len);
	t->out_len += len;
}

int
rome_term_write_pending(RomeTerm *t)
{
	while (t->out_len > 0 && t->fd >= 0) {
		ssize_t n = write(t->fd, t->out, t->out_len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				return 1;
			t->out_len = 0;     /* the child is gone */
			return 0;
		}
		memmove(t->out, t->out + n, t->out_len - n);
		t->out_len -= n;
	}
	return 0;
}

static void
send_output(RomeTerm *t)
{
	if (rome_term_write_pending(t) && t->cb.want_write != NULL)
		t->cb.want_write(t->owner);
}

/* ---- lifecycle ---- */

RomeTerm *
rome_term_new(int rows, int cols, int scrollback, const RomeTheme *theme, const RomeTermCallbacks *cb, void *owner)
{
	RomeTerm *t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;
	if (rows < 2) rows = 2;
	if (cols < 2) cols = 2;
	t->rows = rows;
	t->cols = cols;
	t->fd = -1;
	t->pid = -1;
	t->theme = *theme;
	if (cb != NULL)
		t->cb = *cb;
	t->owner = owner;
	t->drawn_row = -1;
	t->focused = 1;
	t->phase = 1;
	t->cur_visible = 1;
	t->cur_blink = 1;
	t->cur_shape = VTERM_PROP_CURSORSHAPE_BLOCK;
	t->full = 1;
	if (scrollback > 0) {
		t->sb = calloc(scrollback, sizeof(SbLine));
		if (t->sb != NULL)
			t->sb_cap = scrollback;
	}
	if (!alloc_rows(t, rows, cols)) {
		rome_term_free(t);
		return NULL;
	}
	t->vt = vterm_new(rows, cols);
	vterm_set_utf8(t->vt, 1);
	vterm_output_set_callback(t->vt, cb_output, t);
	t->st = vterm_obtain_state(t->vt);
	t->vs = vterm_obtain_screen(t->vt);
	VTermColor fg, bg;
	vterm_color_rgb(&fg, theme->fg >> 16, theme->fg >> 8, theme->fg);
	vterm_color_rgb(&bg, theme->bg >> 16, theme->bg >> 8, theme->bg);
	vterm_state_set_default_colors(t->st, &fg, &bg);
	for (int i = 0; i < 16; i++) {
		VTermColor c;
		uint32_t p = theme->palette[i];
		vterm_color_rgb(&c, p >> 16, p >> 8, p);
		vterm_state_set_palette_color(t->st, i, &c);
	}
	vterm_screen_set_callbacks(t->vs, &screen_cbs, t);
	vterm_screen_set_damage_merge(t->vs, VTERM_DAMAGE_SCROLL);
	vterm_screen_enable_altscreen(t->vs, 1);
	vterm_screen_enable_reflow(t->vs, true);
	vterm_screen_reset(t->vs, 1);
	return t;
}

void
rome_term_free(RomeTerm *t)
{
	if (t == NULL)
		return;
	if (t->fd >= 0)
		close(t->fd);
	if (t->pid > 0)
		kill(t->pid, SIGHUP);
	if (t->vt != NULL)
		vterm_free(t->vt);
	for (int i = 0; i < t->sb_cap; i++)
		free(t->sb[i].cells);
	free(t->sb);
	free(t->dx0);
	free(t->dx1);
	free(t->regen);
	free(t->rowbuf);
	free(t->title);
	free(t->out);
	free(t);
}

int
rome_term_spawn(RomeTerm *t, char *const argv[])
{
	struct winsize ws = { .ws_row = t->rows, .ws_col = t->cols };
	int fd;
	pid_t pid = forkpty(&fd, NULL, NULL, &ws);
	if (pid < 0)
		return -1;
	if (pid == 0) {
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 1);
		setenv("TERM_PROGRAM", "rome", 1);
		unsetenv("LINES");
		unsetenv("COLUMNS");
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		if (argv != NULL && argv[0] != NULL) {
			execvp(argv[0], argv);
			_exit(127);
		}
		struct passwd *pw = getpwuid(getuid());
		const char *shell = getenv("SHELL");
		if (pw != NULL && pw->pw_shell != NULL && pw->pw_shell[0])
			shell = pw->pw_shell;
		if (shell == NULL || !shell[0])
			shell = "/bin/bash";
		const char *home = pw != NULL ? pw->pw_dir : getenv("HOME");
		if (home != NULL && chdir(home) == 0)
			setenv("HOME", home, 1);
		if (pw != NULL) {
			setenv("USER", pw->pw_name, 1);
			setenv("LOGNAME", pw->pw_name, 1);
		}
		setenv("SHELL", shell, 1);
		/* a login shell, as Terminal.app starts it: argv[0] "-bash" */
		const char *base = strrchr(shell, '/');
		base = base ? base + 1 : shell;
		char arg0[256];
		snprintf(arg0, sizeof(arg0), "-%s", base);
		execl(shell, arg0, (char *)NULL);
		_exit(127);
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	t->fd = fd;
	t->pid = pid;
	return fd;
}

pid_t rome_term_pid(RomeTerm *t) { return t->pid; }
int rome_term_fd(RomeTerm *t) { return t->fd; }
int rome_term_rows(RomeTerm *t) { return t->rows; }
int rome_term_cols(RomeTerm *t) { return t->cols; }
int rome_term_mouse_mode(RomeTerm *t) { return t->mouse; }
int rome_term_altscreen(RomeTerm *t) { return t->alt; }
int rome_term_view_offset(RomeTerm *t) { return t->view; }
int rome_term_scrollback_lines(RomeTerm *t) { return t->sb_count; }
int rome_term_cursor_blinks(RomeTerm *t) { return t->cur_blink && t->cur_visible && t->focused && t->view == 0; }

long
rome_term_read(RomeTerm *t, long budget)
{
	char buf[16384];
	long total = 0;
	if (t->fd < 0)
		return -1;
	while (total < budget) {
		ssize_t n = read(t->fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				break;
			return total > 0 ? total : -1;     /* EIO: the child closed the pty */
		}
		if (n == 0)
			return total > 0 ? total : -1;
		vterm_input_write(t->vt, buf, (size_t)n);
		total += n;
	}
	send_output(t);     /* replies to queries (DA, DSR) */
	return total;
}

void
rome_term_feed(RomeTerm *t, const char *bytes, size_t len)
{
	vterm_input_write(t->vt, bytes, len);
}

/* ---- input ---- */

void
rome_term_scroll_to_bottom(RomeTerm *t)
{
	if (t->view != 0) {
		t->view = 0;
		t->full = 1;
	}
}

void
rome_term_key_char(RomeTerm *t, uint32_t c, int mods)
{
	rome_term_scroll_to_bottom(t);
	/* Control characters are sent as bytes. libvterm sends Ctrl with
	 * anything but a-z as a CSI u sequence (keyboard.c,
	 * vterm_keyboard_unichar), which bash and readline do not read. */
	if (mods & VTERM_MOD_CTRL) {
		uint32_t k = c;
		if (k >= 'A' && k <= 'Z')
			k += 'a' - 'A';
		int code = -1;
		if (k >= 'a' && k <= 'z')
			code = (int)(k - 'a' + 1);
		else if (k == '@' || k == ' ' || k == '2')
			code = 0;
		else if (k == '[' || k == '3')
			code = 27;
		else if (k == '\\' || k == '4')
			code = 28;
		else if (k == ']' || k == '5')
			code = 29;
		else if (k == '^' || k == '6')
			code = 30;
		else if (k == '_' || k == '-' || k == '7')
			code = 31;
		else if (k == '?' || k == '8')
			code = 127;
		if (code >= 0) {
			char b[2];
			int n = 0;
			if (mods & VTERM_MOD_ALT)
				b[n++] = 0x1b;
			b[n++] = (char)code;
			cb_output(b, n, t);
			send_output(t);
			return;
		}
	}
	vterm_keyboard_unichar(t->vt, c, (VTermModifier)mods);
	send_output(t);
}

void
rome_term_key(RomeTerm *t, int key, int mods)
{
	rome_term_scroll_to_bottom(t);
	vterm_keyboard_key(t->vt, (VTermKey)key, (VTermModifier)mods);
	send_output(t);
}

void
rome_term_paste(RomeTerm *t, const char *s, size_t len)
{
	rome_term_scroll_to_bottom(t);
	vterm_keyboard_start_paste(t->vt);
	/* newlines are sent as returns, as a typed paste would be */
	for (size_t i = 0; i < len; i++) {
		char c = s[i] == '\n' ? '\r' : s[i];
		cb_output(&c, 1, t);
	}
	vterm_keyboard_end_paste(t->vt);
	send_output(t);
}

void
rome_term_send_raw(RomeTerm *t, const char *bytes, size_t len)
{
	cb_output(bytes, len, t);
	send_output(t);
}

void
rome_term_mouse(RomeTerm *t, int row, int col, int button, int pressed, int mods)
{
	vterm_mouse_move(t->vt, row, col, (VTermModifier)mods);
	if (button > 0)
		vterm_mouse_button(t->vt, button, pressed, (VTermModifier)mods);
	send_output(t);
}

void
rome_term_resize(RomeTerm *t, int rows, int cols, int xpix, int ypix)
{
	if (rows < 2) rows = 2;
	if (cols < 2) cols = 2;
	if (rows == t->rows && cols == t->cols) {
		t->full = 1;
		return;
	}
	if (!alloc_rows(t, rows, cols))
		return;
	t->rows = rows;
	t->cols = cols;
	t->full = 1;
	t->drawn_row = -1;
	RomeRenderer *r = t->r;
	t->r = NULL;            /* no server-side scrolls while reflowing */
	vterm_set_size(t->vt, rows, cols);
	vterm_screen_flush_damage(t->vs);
	t->r = r;
	t->full = 1;
	if (t->fd >= 0) {
		struct winsize ws = { .ws_row = rows, .ws_col = cols, .ws_xpixel = xpix, .ws_ypixel = ypix };
		ioctl(t->fd, TIOCSWINSZ, &ws);  /* the kernel sends SIGWINCH to the foreground group */
	}
}

void
rome_term_scroll_view(RomeTerm *t, int delta)
{
	if (t->alt)
		return;
	int v = t->view + delta;
	if (v < 0) v = 0;
	if (v > t->sb_count) v = t->sb_count;
	if (v != t->view) {
		t->view = v;
		t->full = 1;
	}
}

/* ---- rows ---- */

/* The cells of absolute line `abs` into `out` (cols wide). */
static void
get_line(RomeTerm *t, long long abs, RomeCell *out)
{
	RomeCell blank = { 0, t->theme.fg, t->theme.bg, 0, 1, 0 };
	if (abs >= t->sb_pushed) {
		int row = (int)(abs - t->sb_pushed);
		if (row >= t->rows) {
			for (int i = 0; i < t->cols; i++)
				out[i] = blank;
			return;
		}
		VTermScreenCell vc;
		for (int i = 0; i < t->cols; i++) {
			if (vterm_screen_get_cell(t->vs, (VTermPos){ row, i }, &vc))
				convert_cell(t, &vc, &out[i]);
			else
				out[i] = blank;
		}
		return;
	}
	long long first = t->sb_pushed - t->sb_count;
	int n = 0;
	if (abs >= first && t->sb_cap > 0) {
		SbLine *l = &t->sb[(t->sb_head + (int)(abs - first)) % t->sb_cap];
		n = l->cols < t->cols ? l->cols : t->cols;
		memcpy(out, l->cells, n * sizeof(RomeCell));
		/* a wide character cut at the right edge */
		if (n > 0 && out[n - 1].width == 2 && n == t->cols)
			out[n - 1].width = 1;
	}
	for (int i = n; i < t->cols; i++)
		out[i] = blank;
}

static int
sel_contains(RomeTerm *t, long long line, int col)
{
	if (!t->has_sel)
		return 0;
	if (line < t->sl0 || line > t->sl1)
		return 0;
	if (line == t->sl0 && col < t->sc0)
		return 0;
	if (line == t->sl1 && col >= t->sc1)
		return 0;
	return 1;
}

enum { CUR_NONE, CUR_BLOCK, CUR_BOX, CUR_UNDER, CUR_BAR };

static int
cursor_kind(RomeTerm *t)
{
	if (t->view != 0 || !t->cur_visible)
		return CUR_NONE;
	if (!t->focused)
		return CUR_BOX;
	if (t->cur_blink && !t->phase)
		return CUR_NONE;
	switch (t->cur_shape) {
	case VTERM_PROP_CURSORSHAPE_UNDERLINE: return CUR_UNDER;
	case VTERM_PROP_CURSORSHAPE_BAR_LEFT: return CUR_BAR;
	default: return CUR_BLOCK;
	}
}

static void
fetch_row(RomeTerm *t, int row, RomeCell *out, int kind)
{
	long long abs = t->sb_pushed + row - t->view;
	get_line(t, abs, out);
	if (t->has_sel && abs >= t->sl0 && abs <= t->sl1) {
		for (int i = 0; i < t->cols; i++)
			if (sel_contains(t, abs, i))
				out[i].bg = t->theme.selection;
	}
	if (kind != CUR_NONE && row == t->cur.row && t->cur.col < t->cols) {
		RomeCell *c = &out[t->cur.col];
		if (c->width == 0 && t->cur.col > 0)
			c--;
		switch (kind) {
		case CUR_BLOCK:
			c->fg = c->bg;
			c->bg = t->theme.cursor;
			if (c->width == 2)
				c[1].bg = t->theme.cursor;
			break;
		case CUR_BOX: c->attrs |= ROME_ATTR_CUR_BOX; c->fg = t->theme.cursor; break;
		case CUR_UNDER: c->attrs |= ROME_ATTR_CUR_UNDER; c->fg = t->theme.cursor; break;
		case CUR_BAR: c->attrs |= ROME_ATTR_CUR_BAR; c->fg = t->theme.cursor; break;
		}
	}
}

/* ---- selection ---- */

static void
mark_lines(RomeTerm *t, long long l0, long long l1)
{
	for (int row = 0; row < t->rows; row++) {
		long long abs = t->sb_pushed + row - t->view;
		if (abs >= l0 && abs <= l1)
			mark(t, row, 0, t->cols);
	}
}

static void
set_sel(RomeTerm *t, int has, long long l0, int c0, long long l1, int c1)
{
	if (t->has_sel)
		mark_lines(t, t->sl0, t->sl1);
	t->has_sel = has;
	t->sl0 = l0; t->sc0 = c0;
	t->sl1 = l1; t->sc1 = c1;
	if (has)
		mark_lines(t, l0, l1);
}

void
rome_term_select(RomeTerm *t, int row0, int col0, int row1, int col1)
{
	long long a = t->sb_pushed + row0 - t->view, b = t->sb_pushed + row1 - t->view;
	if (b < a || (b == a && col1 < col0)) {
		long long x = a; a = b; b = x;
		int y = col0; col0 = col1; col1 = y;
	}
	set_sel(t, 1, a, col0, b, col1 + 1);
}

static int
is_word(uint32_t c)
{
	return c > ' ' && !strchr("\"'`()[]{}<>|;,", (int)(c < 128 ? c : 'a'));
}

void
rome_term_select_word(RomeTerm *t, int row, int col)
{
	long long abs = t->sb_pushed + row - t->view;
	get_line(t, abs, t->rowbuf);
	if (col >= t->cols) col = t->cols - 1;
	int a = col, b = col;
	if (is_word(t->rowbuf[col].ch)) {
		while (a > 0 && is_word(t->rowbuf[a - 1].ch)) a--;
		while (b + 1 < t->cols && is_word(t->rowbuf[b + 1].ch)) b++;
	}
	set_sel(t, 1, abs, a, abs, b + 1);
}

void
rome_term_select_line(RomeTerm *t, int row)
{
	long long abs = t->sb_pushed + row - t->view;
	set_sel(t, 1, abs, 0, abs, t->cols);
}

void
rome_term_select_all(RomeTerm *t)
{
	set_sel(t, 1, t->sb_pushed - t->sb_count, 0, t->sb_pushed + t->rows - 1, t->cols);
}

void
rome_term_select_clear(RomeTerm *t)
{
	if (t->has_sel)
		set_sel(t, 0, 0, 0, 0, 0);
}

int rome_term_has_selection(RomeTerm *t) { return t->has_sel; }

static size_t
put_utf8(char *o, uint32_t c)
{
	if (c < 0x80) { o[0] = (char)c; return 1; }
	if (c < 0x800) { o[0] = (char)(0xc0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3f)); return 2; }
	if (c < 0x10000) {
		o[0] = (char)(0xe0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3f));
		o[2] = (char)(0x80 | (c & 0x3f)); return 3;
	}
	o[0] = (char)(0xf0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3f));
	o[2] = (char)(0x80 | ((c >> 6) & 0x3f)); o[3] = (char)(0x80 | (c & 0x3f)); return 4;
}

char *
rome_term_selection_text(RomeTerm *t)
{
	if (!t->has_sel)
		return NULL;
	long long first = t->sb_pushed - t->sb_count;
	long long l0 = t->sl0 < first ? first : t->sl0;
	size_t cap = (size_t)(t->sl1 - l0 + 1) * (t->cols * 4 + 1) + 1, len = 0;
	char *s = malloc(cap);
	if (s == NULL)
		return NULL;
	for (long long l = l0; l <= t->sl1; l++) {
		get_line(t, l, t->rowbuf);
		int c0 = l == t->sl0 ? t->sc0 : 0, c1 = l == t->sl1 ? t->sc1 : t->cols;
		if (c1 > t->cols) c1 = t->cols;
		size_t start = len, end = len;
		for (int i = c0; i < c1; i++) {
			const RomeCell *c = &t->rowbuf[i];
			if (c->width == 0)
				continue;
			len += put_utf8(s + len, c->ch ? c->ch : ' ');
			if (c->ch && c->ch != ' ')
				end = len;
		}
		(void)start;
		len = end;              /* trailing blanks */
		if (l != t->sl1)
			s[len++] = '\n';
	}
	s[len] = 0;
	return s;
}

/* ---- frames ---- */

void
rome_term_set_focus(RomeTerm *t, int focused)
{
	t->focused = focused;
	t->phase = 1;
}

void
rome_term_set_cursor_phase(RomeTerm *t, int on)
{
	t->phase = on;
}

void
rome_term_expose(RomeTerm *t, int x, int y, int w, int h)
{
	if (!t->has_expose) {
		t->has_expose = 1;
		t->ex0 = x; t->ey0 = y; t->ex1 = x + w; t->ey1 = y + h;
		return;
	}
	if (x < t->ex0) t->ex0 = x;
	if (y < t->ey0) t->ey0 = y;
	if (x + w > t->ex1) t->ex1 = x + w;
	if (y + h > t->ey1) t->ey1 = y + h;
}

int
rome_term_needs_render(RomeTerm *t)
{
	if (t->full || t->has_expose)
		return 1;
	if (t->drawn_kind != cursor_kind(t) || t->drawn_row != t->cur.row || t->drawn_col != t->cur.col)
		return 1;
	for (int i = 0; i < t->rows; i++)
		if (t->dx1[i] > t->dx0[i] || t->regen[i])
			return 1;
	return 0;
}

int
rome_term_render(RomeTerm *t, RomeRenderer *r)
{
	t->r = r;
	vterm_screen_flush_damage(t->vs);
	if (r->want_full_redraw) {
		r->want_full_redraw = 0;
		t->full = 1;
	}
	int kind = cursor_kind(t);
	if (kind != t->drawn_kind || t->cur.row != t->drawn_row || t->cur.col != t->drawn_col) {
		if (t->drawn_row >= 0)
			mark(t, t->drawn_row, t->drawn_col - 1, t->drawn_col + 2);
		mark(t, t->cur.row, t->cur.col - 1, t->cur.col + 2);
		t->drawn_row = t->cur.row;
		t->drawn_col = t->cur.col;
		t->drawn_kind = kind;
	}

	RomeFont *f = r->font;
	r->begin(r);
	RomeRect rects[64];
	int nrects = 0, drawn = 0;
	if (t->full) {
		r->clear(r, t->cols, t->rows);
		for (int i = 0; i < t->rows; i++) {
			t->dx0[i] = 0;
			t->dx1[i] = t->cols;
		}
	}
	for (int row = 0; row < t->rows; row++) {
		int x0 = t->dx0[row], x1 = t->dx1[row];
		int present = x1 > x0;
		if (!present && !t->regen[row])
			continue;
		if (!present) {
			x0 = 0;
			x1 = t->cols;
		}
		fetch_row(t, row, t->rowbuf, kind);
		if (x0 > 0 && t->rowbuf[x0].width == 0)
			x0--;
		if (x1 < t->cols && x1 > 0 && t->rowbuf[x1 - 1].width == 2)
			x1++;
		r->draw_row(r, row, x0, x1, t->rowbuf);
		drawn++;
		if (present && !t->full) {
			RomeRect b = { r->padx + x0 * f->cell_w, r->pady + row * f->cell_h, (x1 - x0) * f->cell_w, f->cell_h };
			/* merge with the previous rectangle when it ends on the row above */
			if (nrects > 0 && rects[nrects - 1].y + rects[nrects - 1].h == b.y) {
				RomeRect *p = &rects[nrects - 1];
				int nx0 = p->x < b.x ? p->x : b.x;
				int nx1 = p->x + p->w > b.x + b.w ? p->x + p->w : b.x + b.w;
				p->x = nx0;
				p->w = nx1 - nx0;
				p->h += b.h;
			} else if (nrects < 63) {
				rects[nrects++] = b;
			} else {
				t->full = 1;    /* too many pieces: put the whole window */
			}
		}
		t->dx0[row] = t->dx1[row] = 0;
		t->regen[row] = 0;
	}
	if (t->full) {
		nrects = 1;
		rects[0] = (RomeRect){ 0, 0, r->width, r->height };
	} else if (t->has_expose) {
		rects[nrects++] = (RomeRect){ t->ex0, t->ey0, t->ex1 - t->ex0, t->ey1 - t->ey0 };
	}
	t->full = 0;
	t->has_expose = 0;
	r->present(r, rects, nrects);
	return drawn;
}
