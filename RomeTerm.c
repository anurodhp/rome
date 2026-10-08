/*
 * rome: the terminal core (see RomeTerm.h).
 *
 * libghostty-vt owns the emulation: the screen, the scrollback, reflow, the
 * selection, and the encoding of keys, mouse events and pastes. Rome keeps
 * the pty, the theme and the frame loop.
 *
 * A frame: update the render state from the terminal, then for every row
 * libghostty-vt reports dirty (or that rome itself changed: selection,
 * cursor) build the row's RomeCells, compare them with what the screen
 * shows (`prev`), and hand only the columns that differ to the renderer,
 * which puts only those pixels on the screen. Cost per frame is bounded by
 * the screen, not by how much output arrived. The retained image of the
 * renderer holds everything else, so an expose needs no redraw.
 */
#define _DARWIN_C_SOURCE
#include "RomeTerm.h"

#include <ghostty/vt.h>

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

struct RomeTerm {
	int title_dirty;        /* an OSC title arrived during this feed; delivered once, after it */
	GhosttyTerminal gt;
	GhosttyRenderState rs;
	GhosttyRenderStateRowIterator rit;
	GhosttyRenderStateRowCells rcells;
	GhosttyKeyEncoder kenc;
	GhosttyKeyEvent kev;
	GhosttyMouseEncoder menc;
	GhosttyMouseEvent mev;
	int rows, cols;
	int cell_w, cell_h;
	int fd;
	pid_t pid;
	RomeTermCallbacks cb;
	void *owner;
	RomeTheme theme;
	RomeRenderer *r;

	/* frame state */
	RomeCell *prev;         /* rows x cols: what the screen shows */
	uint8_t *want;          /* rebuild this row whatever the dirty flag says */
	RomeCell *rowbuf;
	int full;               /* clear + redraw + present everything */
	int hint;               /* something may have changed: render */
	int has_expose;
	int ex0, ey0, ex1, ey1;

	/* cursor, as of the last frame */
	int cur_in_view, cur_x, cur_y, cur_visible, cur_blink, cur_style, cur_tail;
	int focused, phase;
	int drawn_kind, drawn_x, drawn_y;   /* what the screen shows; y -1 = none */

	/* selection: the press that started it, in screen rows (stable under scrolling) */
	int sel_clicks;
	int anchor_x;
	int held_button;

	char *out;
	size_t out_len, out_cap, out_off;       /* out_off: already written */
	int reaped;
	uint32_t last_fg, last_bg, cur_color;   /* colours as of the last frame */
	int colors_known;
	int menc_dirty, menc_mode;
	GhosttyTrackedGridRef anchor;           /* where the selection began, follows the text */
};

/* ---- helpers ---- */

static uint32_t
pack(GhosttyColorRgb c)
{
	return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

static GhosttyColorRgb
unpack(uint32_t c)
{
	GhosttyColorRgb r = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
	return r;
}

static int
mode_get(RomeTerm *t, GhosttyMode mode)
{
	GhosttyTerminalModeConfig cfg = { .mode = mode, .value = false };
	if (ghostty_terminal_get(t->gt, GHOSTTY_TERMINAL_DATA_MODE, &cfg) != GHOSTTY_SUCCESS)
		return 0;
	return cfg.value;
}

static int
scrollbar(RomeTerm *t, GhosttyTerminalScrollbar *sb)
{
	memset(sb, 0, sizeof(*sb));
	return ghostty_terminal_get(t->gt, GHOSTTY_TERMINAL_DATA_SCROLLBAR, sb) == GHOSTTY_SUCCESS;
}

void
rome_term_damage_all(RomeTerm *t)
{
	t->full = 1;
	t->hint = 1;
}

static void
want_all(RomeTerm *t)
{
	memset(t->want, 1, t->rows);
	t->hint = 1;
}

static int
alloc_grid(RomeTerm *t, int rows, int cols)
{
	RomeCell *prev = calloc((size_t)rows * cols, sizeof(RomeCell));
	uint8_t *want = calloc(rows, 1);
	RomeCell *rowbuf = calloc((size_t)cols + 1, sizeof(RomeCell));
	if (prev == NULL || want == NULL || rowbuf == NULL) {
		free(prev);
		free(want);
		free(rowbuf);
		return 0;
	}
	free(t->prev);
	free(t->want);
	free(t->rowbuf);
	t->prev = prev;
	t->want = want;
	t->rowbuf = rowbuf;
	return 1;
}

/* ---- output to the pty ---- */

static void
cb_output(const char *s, size_t len, void *user)
{
	RomeTerm *t = user;
	if (t->out_len + len > t->out_cap) {
		size_t cap = t->out_cap ? t->out_cap : 4096;
		if (t->out_off > 0) {                   /* reclaim what was written */
			memmove(t->out, t->out + t->out_off, t->out_len - t->out_off);
			t->out_len -= t->out_off;
			t->out_off = 0;
		}
		while (cap < t->out_len + len)
			cap *= 2;
		if (cap > t->out_cap) {
			char *n = realloc(t->out, cap);
			if (n == NULL)
				return;
			t->out = n;
			t->out_cap = cap;
		}
	}
	memcpy(t->out + t->out_len, s, len);
	t->out_len += len;
}

int
rome_term_write_pending(RomeTerm *t)
{
	while (t->out_len > t->out_off && t->fd >= 0) {
		ssize_t n = write(t->fd, t->out + t->out_off, t->out_len - t->out_off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				return 1;
			t->out_len = t->out_off = 0;        /* the child is gone */
			return 0;
		}
		t->out_off += (size_t)n;
	}
	if (t->out_off >= t->out_len)
		t->out_len = t->out_off = 0;
	return 0;
}

static void
send_output(RomeTerm *t)
{
	if (rome_term_write_pending(t) && t->cb.want_write != NULL)
		t->cb.want_write(t->owner);
}

/* ---- terminal effects ---- */

static void
fx_write_pty(GhosttyTerminal gt, void *ud, const uint8_t *data, size_t len)
{
	RomeTerm *t = ud;
	(void)gt;
	/* A program that never reads its input must not make the replies to its own
	 * queries pile up without bound. Typed input and pastes are not capped. */
	if (t->out_len - t->out_off > 65536)
		return;
	cb_output((const char *)data, len, ud);
}

static void
fx_bell(GhosttyTerminal gt, void *ud)
{
	RomeTerm *t = ud;
	static double last;
	struct timespec ts;
	double now;
	(void)gt;
	/* one beep per 100 ms: a file full of BELs would otherwise beep 100000 times */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
	if (now - last < 100)
		return;
	last = now;
	if (t->cb.bell != NULL)
		t->cb.bell(t->owner);
}

static void
fx_title(GhosttyTerminal gt, void *ud)
{
	(void)gt;
	((RomeTerm *)ud)->title_dirty = 1;
}

static void
deliver_title(RomeTerm *t)
{
	GhosttyString s = { NULL, 0 };
	t->title_dirty = 0;
	if (t->cb.title == NULL || ghostty_terminal_get(t->gt, GHOSTTY_TERMINAL_DATA_TITLE, &s) != GHOSTTY_SUCCESS)
		return;
	char *c = malloc(s.len + 1);
	if (c == NULL)
		return;
	memcpy(c, s.ptr, s.len);
	c[s.len] = 0;
	t->cb.title(t->owner, c);
	free(c);
}

static bool
fx_device_attributes(GhosttyTerminal gt, void *ud, GhosttyDeviceAttributes *out)
{
	(void)gt;
	(void)ud;
	memset(out, 0, sizeof(*out));
	out->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
	out->primary.features[0] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
	out->primary.num_features = 1;
	out->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
	out->secondary.firmware_version = 10;
	return true;
}

/* ---- lifecycle ---- */

static void
make_palette(const RomeTheme *theme, GhosttyColorRgb pal[256])
{
	static const uint8_t lvl[6] = { 0, 95, 135, 175, 215, 255 };
	for (int i = 0; i < 16; i++)
		pal[i] = unpack(theme->palette[i]);
	for (int i = 0; i < 216; i++) {
		pal[16 + i].r = lvl[i / 36];
		pal[16 + i].g = lvl[(i / 6) % 6];
		pal[16 + i].b = lvl[i % 6];
	}
	for (int i = 0; i < 24; i++)
		pal[232 + i] = (GhosttyColorRgb){ (uint8_t)(8 + 10 * i), (uint8_t)(8 + 10 * i), (uint8_t)(8 + 10 * i) };
}

RomeTerm *
rome_term_new(int rows, int cols, int scrollback, const RomeTheme *theme, const RomeTermCallbacks *cb, void *owner)
{
	RomeTerm *t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;
	if (rows < 2) rows = 2;
	if (cols < 2) cols = 2;
	if (rows > 1000) rows = 1000;           /* libghostty takes 16-bit sizes; nothing needs more */
	if (cols > 1000) cols = 1000;
	t->rows = rows;
	t->cols = cols;
	t->cur_color = theme->cursor;
	t->menc_dirty = 1;
	t->cell_w = 8;
	t->cell_h = 16;
	t->fd = -1;
	t->pid = -1;
	t->theme = *theme;
	if (cb != NULL)
		t->cb = *cb;
	t->owner = owner;
	t->drawn_kind = 0;
	t->drawn_y = -1;
	t->focused = 1;
	t->phase = 1;
	t->cur_visible = 1;
	t->cur_blink = 1;
	t->full = 1;
	t->hint = 1;
	if (!alloc_grid(t, rows, cols) ||
	    ghostty_terminal_new(NULL, &t->gt, (uint16_t)cols, (uint16_t)rows) != GHOSTTY_SUCCESS ||
	    ghostty_render_state_new(NULL, &t->rs) != GHOSTTY_SUCCESS ||
	    ghostty_render_state_row_iterator_new(NULL, &t->rit) != GHOSTTY_SUCCESS ||
	    ghostty_render_state_row_cells_new(NULL, &t->rcells) != GHOSTTY_SUCCESS ||
	    ghostty_key_encoder_new(NULL, &t->kenc) != GHOSTTY_SUCCESS ||
	    ghostty_key_event_new(NULL, &t->kev) != GHOSTTY_SUCCESS ||
	    ghostty_mouse_encoder_new(NULL, &t->menc) != GHOSTTY_SUCCESS ||
	    ghostty_mouse_event_new(NULL, &t->mev) != GHOSTTY_SUCCESS) {
		rome_term_free(t);
		return NULL;
	}
	GhosttyTerminal gt = t->gt;
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_USERDATA, t);
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void *)fx_write_pty);
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_BELL, (const void *)fx_bell);
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, (const void *)fx_title);
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES, (const void *)fx_device_attributes);
	GhosttyColorRgb fg = unpack(theme->fg), bg = unpack(theme->bg), pal[256];
	make_palette(theme, pal);
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, pal);
	size_t lines = scrollback > 0 ? (size_t)scrollback : 0, bytes = 16u << 20;
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &lines);
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &bytes);
	uint64_t no_images = 0;
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &no_images);
	bool blink = true;
	ghostty_terminal_set(gt, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_BLINK, &blink);
	return t;
}

/* Children whose terminal was closed while they were still running: SIGHUP was sent, they
 * are collected by rome_term_reap_orphans() once they have gone. */
static pid_t orphans[64];
static int norphans;

static void
orphan_add(pid_t pid)
{
	if (norphans == 64)
		rome_term_reap_orphans();
	if (norphans < 64)
		orphans[norphans++] = pid;
}

void
rome_term_reap_orphans(void)
{
	for (int i = 0; i < norphans; ) {
		int st;
		pid_t r = waitpid(orphans[i], &st, WNOHANG);
		if (r == orphans[i] || (r < 0 && errno == ECHILD))
			orphans[i] = orphans[--norphans];
		else
			i++;
	}
}

int
rome_term_reap(RomeTerm *t, int *status)
{
	int st = 0;
	if (t->pid <= 0 || t->reaped)
		return 0;
	if (waitpid(t->pid, &st, 0) < 0)
		st = 0;
	t->reaped = 1;
	if (status != NULL)
		*status = st;
	return 1;
}

void
rome_term_free(RomeTerm *t)
{
	if (t == NULL)
		return;
	if (t->fd >= 0)
		close(t->fd);
	if (t->pid > 0 && !t->reaped) {
		int st;
		kill(t->pid, SIGHUP);
		if (waitpid(t->pid, &st, WNOHANG) == 0)
			orphan_add(t->pid);     /* still dying: reaped later, no zombie */
	}
	if (t->anchor) ghostty_tracked_grid_ref_free(t->anchor);
	if (t->mev) ghostty_mouse_event_free(t->mev);
	if (t->menc) ghostty_mouse_encoder_free(t->menc);
	if (t->kev) ghostty_key_event_free(t->kev);
	if (t->kenc) ghostty_key_encoder_free(t->kenc);
	if (t->rcells) ghostty_render_state_row_cells_free(t->rcells);
	if (t->rit) ghostty_render_state_row_iterator_free(t->rit);
	if (t->rs) ghostty_render_state_free(t->rs);
	if (t->gt) ghostty_terminal_free(t->gt);
	free(t->prev);
	free(t->want);
	free(t->rowbuf);
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
		/* the shell starts with default signal handling and an empty mask, whatever Rome
		 * inherited (launched from `&`, SIGINT is ignored) */
		{
			static const int sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGCHLD, SIGPIPE, SIGALRM,
			    SIGTSTP, SIGTTIN, SIGTTOU };
			sigset_t none;
			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, NULL);
			for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
				signal(sigs[i], SIG_DFL);
		}
		if (argv != NULL && argv[0] != NULL) {
			execvp(argv[0], argv);
			(void)!write(2, "rome: cannot execute the command\r\n", 34);
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
		(void)!write(2, "rome: cannot execute the shell\r\n", 32);
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

int
rome_term_mouse_mode(RomeTerm *t)
{
	if (mode_get(t, GHOSTTY_MODE_ANY_MOUSE))
		return ROME_MOUSE_MOVE;
	if (mode_get(t, GHOSTTY_MODE_BUTTON_MOUSE))
		return ROME_MOUSE_DRAG;
	if (mode_get(t, GHOSTTY_MODE_NORMAL_MOUSE) || mode_get(t, GHOSTTY_MODE_X10_MOUSE))
		return ROME_MOUSE_CLICK;
	return ROME_MOUSE_NONE;
}

int
rome_term_altscreen(RomeTerm *t)
{
	GhosttyTerminalScreen s = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
	ghostty_terminal_get(t->gt, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &s);
	return s == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
}

int
rome_term_scrollback_lines(RomeTerm *t)
{
	GhosttyTerminalScrollbar sb;
	if (!scrollbar(t, &sb))
		return 0;
	return sb.total > sb.len ? (int)(sb.total - sb.len) : 0;
}

int
rome_term_view_offset(RomeTerm *t)
{
	GhosttyTerminalScrollbar sb;
	if (!scrollbar(t, &sb) || sb.total < sb.len)
		return 0;
	unsigned long long bottom = sb.total - sb.len;
	return sb.offset < bottom ? (int)(bottom - sb.offset) : 0;
}

int
rome_term_cursor_blinks(RomeTerm *t)
{
	return t->cur_blink && t->cur_visible && t->focused && t->cur_in_view;
}

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
		ghostty_terminal_vt_write(t->gt, (const uint8_t *)buf, (size_t)n);
		total += n;
	}
	if (total > 0)
		t->hint = 1;
	send_output(t);     /* replies to queries (DA, DSR) */
	return total;
}

void
rome_term_feed(RomeTerm *t, const char *bytes, size_t len)
{
	ghostty_terminal_vt_write(t->gt, (const uint8_t *)bytes, len);
	if (t->title_dirty)
		deliver_title(t);
	t->hint = 1;
	send_output(t);
}

/* ---- input ---- */

void
rome_term_scroll_to_bottom(RomeTerm *t)
{
	GhosttyTerminalScrollViewport b = { .tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM };
	GhosttyTerminalScrollbar sb;
	if (scrollbar(t, &sb) && sb.total >= sb.len && sb.offset == sb.total - sb.len)
		return;
	ghostty_terminal_scroll_viewport(t->gt, b);
	t->hint = 1;
}

static GhosttyMods
ghostty_mods(int mods)
{
	GhosttyMods m = 0;
	if (mods & ROME_MOD_SHIFT) m |= GHOSTTY_MODS_SHIFT;
	if (mods & ROME_MOD_CTRL) m |= GHOSTTY_MODS_CTRL;
	if (mods & ROME_MOD_ALT) m |= GHOSTTY_MODS_ALT;
	return m;
}

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

/* The physical key of an ASCII character on a US layout, and whether Shift is what makes it. */
static GhosttyKey
ascii_key(uint32_t c, int *shifted, uint32_t *base)
{
	static const char *shift_syms = "!@#$%^&*()_+{}|:\"<>?~";
	static const char *base_syms  = "1234567890-=[]\\;',./`";
	*shifted = 0;
	*base = c;
	if (c >= 'A' && c <= 'Z') {
		*shifted = 1;
		*base = c + ('a' - 'A');
		return (GhosttyKey)(GHOSTTY_KEY_A + (c - 'A'));
	}
	if (c >= 'a' && c <= 'z')
		return (GhosttyKey)(GHOSTTY_KEY_A + (c - 'a'));
	if (c >= '0' && c <= '9')
		return (GhosttyKey)(GHOSTTY_KEY_DIGIT_0 + (c - '0'));
	if (c < 128 && c != 0 && strchr(shift_syms, (int)c) != NULL) {
		static const char pairs[][2] = { {'!','1'},{'@','2'},{'#','3'},{'$','4'},{'%','5'},{'^','6'},{'&','7'},{'*','8'},
		    {'(','9'},{')','0'},{'_','-'},{'+','='},{'{','['},{'}',']'},{'|','\\'},{':',';'},{'"','\''},{'<',','},
		    {'>','.'},{'?','/'},{'~','`'} };
		for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
			if ((uint32_t)pairs[i][0] == c) {
				uint32_t b2 = (unsigned char)pairs[i][1];
				int s2;
				uint32_t b3;
				*shifted = 1;
				*base = b2;
				return ascii_key(b2, &s2, &b3);
			}
		}
	}
	(void)base_syms;
	switch (c) {
	case ' ': return GHOSTTY_KEY_SPACE;
	case '-': return GHOSTTY_KEY_MINUS;
	case '=': return GHOSTTY_KEY_EQUAL;
	case '[': return GHOSTTY_KEY_BRACKET_LEFT;
	case ']': return GHOSTTY_KEY_BRACKET_RIGHT;
	case '\\': return GHOSTTY_KEY_BACKSLASH;
	case ';': return GHOSTTY_KEY_SEMICOLON;
	case '\'': return GHOSTTY_KEY_QUOTE;
	case ',': return GHOSTTY_KEY_COMMA;
	case '.': return GHOSTTY_KEY_PERIOD;
	case '/': return GHOSTTY_KEY_SLASH;
	case '`': return GHOSTTY_KEY_BACKQUOTE;
	}
	return GHOSTTY_KEY_UNIDENTIFIED;
}

/* With the Kitty keyboard protocol on (or modifyOtherKeys), a character key is reported as a key
 * event, not as the raw bytes. Returns 1 if the encoder produced the bytes. */
static int
key_char_via_encoder(RomeTerm *t, uint32_t c, int mods, GhosttyKeyAction action)
{
	uint8_t flags = 0;
	char out[64], text[8];
	size_t n = 0, tn = 0;
	int shifted;
	uint32_t base;
	GhosttyKey k;
	ghostty_terminal_get(t->gt, GHOSTTY_TERMINAL_DATA_KITTY_KEYBOARD_FLAGS, &flags);
	if (flags == 0 || c >= 0xd800)
		return 0;
	k = ascii_key(c, &shifted, &base);
	ghostty_key_encoder_setopt_from_terminal(t->kenc, t->gt);
	ghostty_key_event_set_action(t->kev, action);
	ghostty_key_event_set_key(t->kev, k);
	ghostty_key_event_set_mods(t->kev, ghostty_mods(mods | (shifted ? ROME_MOD_SHIFT : 0)));
	ghostty_key_event_set_consumed_mods(t->kev, shifted ? GHOSTTY_MODS_SHIFT : 0);
	ghostty_key_event_set_composing(t->kev, false);
	ghostty_key_event_set_unshifted_codepoint(t->kev, k == GHOSTTY_KEY_UNIDENTIFIED ? c : base);
	/* the text the key produces: none while Control is held */
	if (!(mods & ROME_MOD_CTRL))
		tn = put_utf8(text, c);
	ghostty_key_event_set_utf8(t->kev, tn ? text : NULL, tn);
	if (ghostty_key_encoder_encode(t->kenc, t->kev, out, sizeof(out), &n) != GHOSTTY_SUCCESS || n == 0)
		return 0;
	cb_output(out, n, t);
	send_output(t);
	return 1;
}

void
rome_term_key_char(RomeTerm *t, uint32_t c, int mods)
{
	char b[8];
	size_t n = 0;
	if ((c >= 0xd800 && c < 0xe000) || c > 0x10ffff)
		return;         /* not a character */
	rome_term_scroll_to_bottom(t);
	if (key_char_via_encoder(t, c, mods, GHOSTTY_KEY_ACTION_PRESS))
		return;
	/* Control characters are sent as bytes (Ctrl-C is 0x03), with an ESC in
	 * front for Alt. */
	if (mods & ROME_MOD_CTRL) {
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
		else if (k == '_' || k == '-' || k == '/' || k == '7')
			code = 31;
		else if (k == '?' || k == '8')
			code = 127;
		if (code >= 0) {
			if (mods & ROME_MOD_ALT)
				b[n++] = 0x1b;
			b[n++] = (char)code;
			cb_output(b, n, t);
			send_output(t);
			return;
		}
	}
	if (mods & ROME_MOD_ALT)
		b[n++] = 0x1b;
	n += put_utf8(b + n, c);
	cb_output(b, n, t);
	send_output(t);
}

static GhosttyKey
map_key(int key)
{
	if (key >= ROME_KEY_F(1) && key <= ROME_KEY_F(25))
		return (GhosttyKey)(GHOSTTY_KEY_F1 + (key - ROME_KEY_F(1)));
	switch (key) {
	case ROME_KEY_ENTER: return GHOSTTY_KEY_ENTER;
	case ROME_KEY_TAB: return GHOSTTY_KEY_TAB;
	case ROME_KEY_BACKSPACE: return GHOSTTY_KEY_BACKSPACE;
	case ROME_KEY_ESCAPE: return GHOSTTY_KEY_ESCAPE;
	case ROME_KEY_UP: return GHOSTTY_KEY_ARROW_UP;
	case ROME_KEY_DOWN: return GHOSTTY_KEY_ARROW_DOWN;
	case ROME_KEY_LEFT: return GHOSTTY_KEY_ARROW_LEFT;
	case ROME_KEY_RIGHT: return GHOSTTY_KEY_ARROW_RIGHT;
	case ROME_KEY_INS: return GHOSTTY_KEY_INSERT;
	case ROME_KEY_DEL: return GHOSTTY_KEY_DELETE;
	case ROME_KEY_HOME: return GHOSTTY_KEY_HOME;
	case ROME_KEY_END: return GHOSTTY_KEY_END;
	case ROME_KEY_PAGEUP: return GHOSTTY_KEY_PAGE_UP;
	case ROME_KEY_PAGEDOWN: return GHOSTTY_KEY_PAGE_DOWN;
	}
	return GHOSTTY_KEY_UNIDENTIFIED;
}

static void
key_special(RomeTerm *t, int key, int mods, GhosttyKeyAction action)
{
	char buf[64];
	size_t n = 0;
	GhosttyKey k = map_key(key);
	if (k == GHOSTTY_KEY_UNIDENTIFIED)
		return;
	if (action != GHOSTTY_KEY_ACTION_RELEASE)
		rome_term_scroll_to_bottom(t);
	/* cursor-key mode, the Kitty flags and so on follow the terminal */
	ghostty_key_encoder_setopt_from_terminal(t->kenc, t->gt);
	ghostty_key_event_set_action(t->kev, action);
	ghostty_key_event_set_key(t->kev, k);
	ghostty_key_event_set_mods(t->kev, ghostty_mods(mods));
	ghostty_key_event_set_consumed_mods(t->kev, 0);
	ghostty_key_event_set_composing(t->kev, false);
	ghostty_key_event_set_utf8(t->kev, NULL, 0);
	if (ghostty_key_encoder_encode(t->kenc, t->kev, buf, sizeof(buf), &n) == GHOSTTY_SUCCESS && n > 0) {
		cb_output(buf, n, t);
		send_output(t);
	}
}

void
rome_term_key(RomeTerm *t, int key, int mods)
{
	key_special(t, key, mods, GHOSTTY_KEY_ACTION_PRESS);
}

/* Press, repeat and release of a special key (`key`) or a character (`c`). Without the Kitty
 * keyboard protocol asking for them, a release sends nothing and a repeat is a press. */
void
rome_term_key_event(RomeTerm *t, int action, int key, uint32_t c, int mods)
{
	GhosttyKeyAction a = action == ROME_KEY_RELEASE ? GHOSTTY_KEY_ACTION_RELEASE :
	    action == ROME_KEY_REPEAT ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS;
	if (key != ROME_KEY_NONE) {
		key_special(t, key, mods, a);
		return;
	}
	if (c == 0 || (c >= 0xd800 && c < 0xe000) || c > 0x10ffff)
		return;
	if (a == GHOSTTY_KEY_ACTION_PRESS)
		rome_term_key_char(t, c, mods);
	else if (!key_char_via_encoder(t, c, mods, a) && a == GHOSTTY_KEY_ACTION_REPEAT)
		rome_term_key_char(t, c, mods);
}

/* Would pasting this text run commands in a program that did not ask for a paste? (no bracketed
 * paste mode, and the text has line breaks) */
int
rome_term_paste_needs_confirm(RomeTerm *t, const char *s, size_t len)
{
	if (len == 0 || mode_get(t, GHOSTTY_MODE_BRACKETED_PASTE))
		return 0;
	return !ghostty_paste_is_safe(s, len);
}

void
rome_term_paste(RomeTerm *t, const char *s, size_t len)
{
	rome_term_scroll_to_bottom(t);
	if (len == 0)
		return;
	size_t cap = len + 64, w = 0;
	char *data = malloc(len), *buf = malloc(cap);
	if (data == NULL || buf == NULL) {
		free(data);
		free(buf);
		return;
	}
	memcpy(data, s, len);
	/* newlines go out as returns, as a typed paste would be; the markers
	 * are added when the application asked for bracketed paste */
	if (ghostty_paste_encode(data, len, mode_get(t, GHOSTTY_MODE_BRACKETED_PASTE), buf, cap, &w) == GHOSTTY_SUCCESS) {
		cb_output(buf, w, t);
		send_output(t);
	}
	free(data);
	free(buf);
}

void
rome_term_send_raw(RomeTerm *t, const char *bytes, size_t len)
{
	cb_output(bytes, len, t);
	send_output(t);
}

static GhosttyMouseButton
map_button(int button)
{
	switch (button) {
	case 1: return GHOSTTY_MOUSE_BUTTON_LEFT;
	case 2: return GHOSTTY_MOUSE_BUTTON_MIDDLE;
	case 3: return GHOSTTY_MOUSE_BUTTON_RIGHT;
	case 4: return GHOSTTY_MOUSE_BUTTON_FOUR;
	case 5: return GHOSTTY_MOUSE_BUTTON_FIVE;
	}
	return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
}

void
rome_term_mouse(RomeTerm *t, int row, int col, int button, int pressed, int mods)
{
	char buf[64];
	size_t n = 0;
	GhosttyMouseEncoderSize sz = {
		.size = sizeof(sz),
		.screen_width = (uint32_t)(t->cols * t->cell_w), .screen_height = (uint32_t)(t->rows * t->cell_h),
		.cell_width = (uint32_t)t->cell_w, .cell_height = (uint32_t)t->cell_h,
	};
	/* Each of these clears the encoder's "last cell", which is what lets it drop motion that did not
	 * change cell, so only reconfigure on a press or release, or when the application changed
	 * its mouse mode. */
	{
		int mm = rome_term_mouse_mode(t);
		if (button > 0 || mm != t->menc_mode || t->menc_dirty) {
			bool track = true;
			ghostty_mouse_encoder_setopt_from_terminal(t->menc, t->gt);
			ghostty_mouse_encoder_setopt(t->menc, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &sz);
			ghostty_mouse_encoder_setopt(t->menc, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &track);
			t->menc_mode = mm;
			t->menc_dirty = 0;
		}
	}
	{
		bool held = t->held_button > 0 || (button > 0 && button <= 3 && pressed);
		ghostty_mouse_encoder_setopt(t->menc, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &held);
	}
	if (button > 0) {
		ghostty_mouse_event_set_action(t->mev, pressed ? GHOSTTY_MOUSE_ACTION_PRESS : GHOSTTY_MOUSE_ACTION_RELEASE);
		ghostty_mouse_event_set_button(t->mev, map_button(button));
		if (button <= 3)
			t->held_button = pressed ? button : 0;
	} else {
		ghostty_mouse_event_set_action(t->mev, GHOSTTY_MOUSE_ACTION_MOTION);
		if (t->held_button > 0)
			ghostty_mouse_event_set_button(t->mev, map_button(t->held_button));
		else
			ghostty_mouse_event_clear_button(t->mev);
	}
	ghostty_mouse_event_set_mods(t->mev, ghostty_mods(mods));
	ghostty_mouse_event_set_position(t->mev, (GhosttyMousePosition){
	    .x = (float)(col * t->cell_w + t->cell_w / 2), .y = (float)(row * t->cell_h + t->cell_h / 2) });
	if (ghostty_mouse_encoder_encode(t->menc, t->mev, buf, sizeof(buf), &n) == GHOSTTY_SUCCESS && n > 0) {
		cb_output(buf, n, t);
		send_output(t);
	}
}

void
rome_term_resize(RomeTerm *t, int rows, int cols, int xpix, int ypix)
{
	if (rows < 2) rows = 2;
	if (cols < 2) cols = 2;
	if (rows > 1000) rows = 1000;
	if (cols > 1000) cols = 1000;
	t->menc_dirty = 1;
	if (xpix >= cols) t->cell_w = xpix / cols;
	if (ypix >= rows) t->cell_h = ypix / rows;
	t->full = 1;
	t->hint = 1;
	if (rows == t->rows && cols == t->cols)
		return;
	if (!alloc_grid(t, rows, cols))
		return;
	t->rows = rows;
	t->cols = cols;
	t->drawn_y = -1;
	ghostty_terminal_resize(t->gt, (uint16_t)cols, (uint16_t)rows, (uint32_t)t->cell_w, (uint32_t)t->cell_h);
	if (t->fd >= 0) {
		struct winsize ws = { .ws_row = rows, .ws_col = cols, .ws_xpixel = xpix, .ws_ypixel = ypix };
		ioctl(t->fd, TIOCSWINSZ, &ws);  /* the kernel sends SIGWINCH to the foreground group */
	}
}

void
rome_term_scroll_view(RomeTerm *t, int delta)
{
	if (delta == 0 || rome_term_altscreen(t))
		return;
	if (delta > 1000000) delta = 1000000;
	if (delta < -1000000) delta = -1000000;
	GhosttyTerminalScrollViewport b = { .tag = GHOSTTY_SCROLL_VIEWPORT_DELTA, .value = { .delta = -(intptr_t)delta } };
	ghostty_terminal_scroll_viewport(t->gt, b);
	t->hint = 1;
}

/* ---- selection ---- */

static int
viewport_ref(RomeTerm *t, int row, int col, GhosttyGridRef *ref)
{
	GhosttyPoint p = { .tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = { .coordinate = { .x = (uint16_t)col, .y = (uint32_t)row } } };
	*ref = (GhosttyGridRef)GHOSTTY_INIT_SIZED(GhosttyGridRef);
	return ghostty_terminal_grid_ref(t->gt, p, ref) == GHOSTTY_SUCCESS;
}

/* The press that began the selection, as a reference the terminal keeps pointing at the same
 * text while output scrolls, scrollback is pruned and the screen reflows. */
static int
anchor_ref(RomeTerm *t, GhosttyGridRef *ref)
{
	*ref = (GhosttyGridRef)GHOSTTY_INIT_SIZED(GhosttyGridRef);
	return t->anchor != NULL && ghostty_tracked_grid_ref_has_value(t->anchor) &&
	    ghostty_tracked_grid_ref_snapshot(t->anchor, ref) == GHOSTTY_SUCCESS;
}

static void
set_selection(RomeTerm *t, const GhosttySelection *sel)
{
	ghostty_terminal_set(t->gt, GHOSTTY_TERMINAL_OPT_SELECTION, sel);
	want_all(t);
}

void
rome_term_select_clear(RomeTerm *t)
{
	if (rome_term_has_selection(t)) {
		ghostty_terminal_set(t->gt, GHOSTTY_TERMINAL_OPT_SELECTION, NULL);
		want_all(t);
	}
}

int
rome_term_has_selection(RomeTerm *t)
{
	GhosttySelection s = GHOSTTY_INIT_SIZED(GhosttySelection);
	return ghostty_terminal_get(t->gt, GHOSTTY_TERMINAL_DATA_SELECTION, &s) == GHOSTTY_SUCCESS;
}

void
rome_term_select_begin(RomeTerm *t, int row, int col, int clicks)
{
	GhosttyTerminalScrollbar sb;
	GhosttyGridRef ref;
	GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
	t->sel_clicks = clicks < 1 ? 1 : clicks;
	t->anchor_x = col;
	if (t->anchor != NULL) {
		ghostty_tracked_grid_ref_free(t->anchor);
		t->anchor = NULL;
	}
	{
		GhosttyPoint p = { .tag = GHOSTTY_POINT_TAG_VIEWPORT,
		    .value = { .coordinate = { .x = (uint16_t)col, .y = (uint32_t)row } } };
		ghostty_terminal_grid_ref_track(t->gt, p, &t->anchor);
	}
	(void)sb;
	if (t->sel_clicks == 1) {
		rome_term_select_clear(t);
		return;
	}
	if (!viewport_ref(t, row, col, &ref))
		return;
	if (t->sel_clicks == 2) {
		GhosttyTerminalSelectWordOptions o = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
		o.ref = ref;
		if (ghostty_terminal_select_word(t->gt, &o, &sel) == GHOSTTY_SUCCESS)
			set_selection(t, &sel);
		else
			rome_term_select_clear(t);      /* blank cells: nothing to select, not the old selection */
	} else {
		GhosttyTerminalSelectLineOptions o = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectLineOptions);
		o.ref = ref;
		if (ghostty_terminal_select_line(t->gt, &o, &sel) == GHOSTTY_SUCCESS)
			set_selection(t, &sel);
		else
			rome_term_select_clear(t);
	}
}

void
rome_term_select_extend(RomeTerm *t, int row, int col)
{
	GhosttyGridRef cur, anc;
	GhosttyPointCoordinate ap = { 0, 0 }, cp = { 0, 0 };
	GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
	int backward;
	if (!viewport_ref(t, row, col, &cur) || !anchor_ref(t, &anc))
		return;
	/* is the pointer before the press? (both as rows from the top of the whole screen) */
	GhosttyPoint cpt = { .tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = { .coordinate = { .x = (uint16_t)col, .y = (uint32_t)row } } };
	(void)cpt;
	ghostty_tracked_grid_ref_point(t->anchor, GHOSTTY_POINT_TAG_SCREEN, &ap);
	{
		GhosttyTerminalScrollbar sb;
		cp.y = (uint32_t)row + (scrollbar(t, &sb) ? (uint32_t)sb.offset : 0);
		cp.x = (uint16_t)col;
	}
	backward = cp.y < ap.y || (cp.y == ap.y && cp.x < ap.x);
	if (t->sel_clicks <= 1) {
		sel.start = anc;
		sel.end = cur;
		set_selection(t, &sel);
		return;
	}
	if (t->sel_clicks == 2) {
		/* the word under the press and the word under the pointer, joined */
		GhosttySelection a = GHOSTTY_INIT_SIZED(GhosttySelection), b = GHOSTTY_INIT_SIZED(GhosttySelection);
		GhosttyTerminalSelectWordBetweenOptions o = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordBetweenOptions);
		o.start = anc;
		o.end = cur;
		if (ghostty_terminal_select_word_between(t->gt, &o, &a) != GHOSTTY_SUCCESS)
			return;
		o.start = cur;
		o.end = anc;
		if (ghostty_terminal_select_word_between(t->gt, &o, &b) != GHOSTTY_SUCCESS)
			return;
		if (backward) {
			sel.start = b.start;
			sel.end = a.end;
		} else {
			sel.start = a.start;
			sel.end = b.end;
		}
		set_selection(t, &sel);
		return;
	}
	GhosttySelection la = GHOSTTY_INIT_SIZED(GhosttySelection), lc = GHOSTTY_INIT_SIZED(GhosttySelection);
	GhosttyTerminalSelectLineOptions lo = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectLineOptions);
	lo.ref = anc;
	if (ghostty_terminal_select_line(t->gt, &lo, &la) != GHOSTTY_SUCCESS)
		return;
	lo.ref = cur;
	if (ghostty_terminal_select_line(t->gt, &lo, &lc) != GHOSTTY_SUCCESS)
		return;
	if (backward) {
		sel.start = la.end;
		sel.end = lc.start;
	} else {
		sel.start = la.start;
		sel.end = lc.end;
	}
	set_selection(t, &sel);
}

void
rome_term_select_all(RomeTerm *t)
{
	GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
	if (ghostty_terminal_select_all(t->gt, &sel) == GHOSTTY_SUCCESS)
		set_selection(t, &sel);
}

char *
rome_term_selection_text(RomeTerm *t)
{
	GhosttyTerminalSelectionFormatOptions o = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
	uint8_t *p = NULL;
	size_t n = 0;
	o.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
	o.trim = true;
	o.unwrap = true;        /* a soft-wrapped line is one line in the clipboard */
	o.selection = NULL;     /* the terminal's own */
	if (ghostty_terminal_selection_format_alloc(t->gt, NULL, o, &p, &n) != GHOSTTY_SUCCESS || p == NULL)
		return NULL;
	char *s = malloc(n + 1);
	if (s != NULL) {
		memcpy(s, p, n);
		s[n] = 0;
	}
	ghostty_free(NULL, p, n);
	return s;
}

/* ---- frames ---- */

void
rome_term_set_focus(RomeTerm *t, int focused)
{
	focused = !!focused;
	if (focused != t->focused && mode_get(t, GHOSTTY_MODE_FOCUS_EVENT)) {
		cb_output(focused ? "\033[I" : "\033[O", 3, t);
		send_output(t);
	}
	t->focused = focused;
	t->phase = 1;
	t->hint = 1;
}

void
rome_term_set_cursor_phase(RomeTerm *t, int on)
{
	t->phase = on;
	t->hint = 1;
}

void
rome_term_expose(RomeTerm *t, int x, int y, int w, int h)
{
	t->hint = 1;
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
	return t->full || t->has_expose || t->hint;
}

enum { CUR_NONE, CUR_BLOCK, CUR_BOX, CUR_UNDER, CUR_BAR };

static int
cursor_kind(RomeTerm *t)
{
	if (!t->cur_in_view || !t->cur_visible)
		return CUR_NONE;
	if (!t->focused)
		return CUR_BOX;
	if (t->cur_blink && !t->phase)
		return CUR_NONE;
	switch (t->cur_style) {
	case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE: return CUR_UNDER;
	case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR: return CUR_BAR;
	case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW: return CUR_BOX;
	default: return CUR_BLOCK;
	}
}

static uint32_t
dim(uint32_t fg, uint32_t bg)
{
	uint32_t r = ((fg >> 16 & 255) * 2 + (bg >> 16 & 255)) / 3;
	uint32_t g = ((fg >> 8 & 255) * 2 + (bg >> 8 & 255)) / 3;
	uint32_t b = ((fg & 255) * 2 + (bg & 255)) / 3;
	return r << 16 | g << 8 | b;
}

/* An extra code point of a grapheme cluster that is drawn over its base character: the combining
 * marks (accents, Hebrew points, Arabic harakat, Thai and Indic vowel signs, ...). Not drawn: joiners,
 * variation selectors, skin-tone modifiers, tags (nothing to see, a missing-glyph box if drawn), the
 * emoji of a ZWJ sequence (would overprint the first), or conjoining Hangul jamo (need shaping). */
static int
draws_as_mark(uint32_t cp)
{
	if (cp == 0x34f || (cp >= 0x1100 && cp <= 0x11ff) || (cp >= 0x180b && cp <= 0x180f))
		return 0;
	return (cp >= 0x300 && cp < 0x2000) || (cp >= 0x20d0 && cp <= 0x20ff) || (cp >= 0xfe20 && cp <= 0xfe2f);
}

/* The cells of the iterator's current row into t->rowbuf, selection applied. */
static void
build_row(RomeTerm *t, const GhosttyRenderStateColors *col)
{
	RomeCell *out = t->rowbuf;
	uint32_t dfg = pack(col->foreground), dbg = pack(col->background);
	GhosttyRenderStateRowSelection rsel = GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
	int has_sel = 0, x = 0, has_graphemes = 0;
	has_sel = ghostty_render_state_row_get(t->rit, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &rsel) == GHOSTTY_SUCCESS;
	{
		GhosttyRow raw_row = 0;
		bool g = false;
		if (ghostty_render_state_row_get(t->rit, GHOSTTY_RENDER_STATE_ROW_DATA_RAW, &raw_row) == GHOSTTY_SUCCESS &&
		    ghostty_row_get(raw_row, GHOSTTY_ROW_DATA_GRAPHEME, &g) == GHOSTTY_SUCCESS)
			has_graphemes = g;
	}
	if (ghostty_render_state_row_get(t->rit, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &t->rcells) == GHOSTTY_SUCCESS) {
		while (x < t->cols && ghostty_render_state_row_cells_next(t->rcells)) {
			RomeCell *c = &out[x];
			GhosttyCell raw = 0;
			uint32_t cp = 0;
			int wide = GHOSTTY_CELL_WIDE_NARROW;
			bool styled = false;
			GhosttyColorRgb rgb;
			uint32_t fg = dfg, bg = dbg;
			uint8_t attrs = 0;
			ghostty_render_state_row_cells_get(t->rcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
			ghostty_cell_get(raw, GHOSTTY_CELL_DATA_CODEPOINT, &cp);
			ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
			ghostty_cell_get(raw, GHOSTTY_CELL_DATA_HAS_STYLING, &styled);
			if (ghostty_render_state_row_cells_get(t->rcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &rgb) == GHOSTTY_SUCCESS)
				bg = pack(rgb);
			if (styled) {
				GhosttyStyle st = GHOSTTY_INIT_SIZED(GhosttyStyle);
				if (ghostty_render_state_row_cells_get(t->rcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &rgb) == GHOSTTY_SUCCESS)
					fg = pack(rgb);
				if (ghostty_render_state_row_cells_get(t->rcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &st) == GHOSTTY_SUCCESS) {
					if (st.faint)
						fg = dim(fg, bg);
					if (st.inverse) {
						uint32_t v = fg;
						fg = bg;
						bg = v;
					}
					if (st.invisible)
						fg = bg;
					attrs = (st.bold ? ROME_ATTR_BOLD : 0) | (st.italic ? ROME_ATTR_ITALIC : 0) |
					    (st.underline ? ROME_ATTR_UNDERLINE : 0) | (st.strikethrough ? ROME_ATTR_STRIKE : 0);
				}
			}
			c->mark[0] = c->mark[1] = 0;
			if (has_graphemes) {
				uint32_t glen = 0, gbuf[16];
				ghostty_render_state_row_cells_get(t->rcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &glen);
				if (glen > 1 && glen <= 16 &&
				    ghostty_render_state_row_cells_get(t->rcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, gbuf) == GHOSTTY_SUCCESS) {
					int nm = 0;
					for (uint32_t gi = 1; gi < glen && nm < 2; gi++)
						if (draws_as_mark(gbuf[gi]))
							c->mark[nm++] = gbuf[gi];
				}
			}
			c->ch = cp;
			c->width = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : wide == GHOSTTY_CELL_WIDE_SPACER_TAIL ? 0 : 1;
			if (c->width == 0)
				c->ch = 0;
			c->fg = fg;
			c->bg = bg;
			c->attrs = attrs;
			c->pad = 0;
			x++;
		}
	}
	for (; x < t->cols; x++)
		out[x] = (RomeCell){ .ch = 0, .fg = dfg, .bg = dbg, .width = 1 };
	if (has_sel) {
		for (int i = rsel.start_x; i <= (int)rsel.end_x && i < t->cols; i++)
			out[i].bg = t->theme.selection;
	}
}

static void
apply_cursor(RomeTerm *t, int kind)
{
	RomeCell *c = &t->rowbuf[t->cur_x < t->cols ? t->cur_x : t->cols - 1];
	if (c->width == 0 && t->cur_x > 0)
		c--;
	switch (kind) {
	case CUR_BLOCK:
		c->fg = c->bg;
		c->bg = t->cur_color;
		if (c->width == 2)
			c[1].bg = t->cur_color;
		break;
	case CUR_BOX: c->attrs |= ROME_ATTR_CUR_BOX; c->fg = t->cur_color; break;
	case CUR_UNDER: c->attrs |= ROME_ATTR_CUR_UNDER; c->fg = t->cur_color; break;
	case CUR_BAR: c->attrs |= ROME_ATTR_CUR_BAR; c->fg = t->cur_color; break;
	}
}

static int
cell_eq(const RomeCell *a, const RomeCell *b)
{
	return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg && a->attrs == b->attrs && a->width == b->width &&
	    a->mark[0] == b->mark[0] && a->mark[1] == b->mark[1];
}

int
rome_term_render(RomeTerm *t, RomeRenderer *r)
{
	GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
	GhosttyRenderStateCursor cur = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
	GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
	RomeFont *f = r->font;
	RomeRect rects[64];
	int nrects = 0, drawn = 0, y = 0;

	t->r = r;
	if (ghostty_render_state_update(t->rs, t->gt) != GHOSTTY_SUCCESS)
		return 0;
	{
		/* The terminal can change its own size (DECCOLM, ESC[?3h): Rome's grid, the pty and the
		 * window are the truth, so put it back. */
		uint16_t rc = 0, rr = 0;
		ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_COLS, &rc);
		ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_ROWS, &rr);
		if (rc != t->cols || rr != t->rows) {
			ghostty_terminal_resize(t->gt, (uint16_t)t->cols, (uint16_t)t->rows, (uint32_t)t->cell_w, (uint32_t)t->cell_h);
			if (ghostty_render_state_update(t->rs, t->gt) != GHOSTTY_SUCCESS)
				return 0;
			t->full = 1;
		}
	}
	if (r->want_full_redraw) {
		r->want_full_redraw = 0;
		t->full = 1;
	}
	ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);
	/* A global change (selection, colours, scrolling the viewport): look at
	 * every row, but still draw and put only the cells that differ. */
	if (dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL)
		memset(t->want, 1, t->rows);
	ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);
	{
		/* OSC 10/11/12 change the default colours without marking anything dirty: every cell
		 * drawn with a default colour, and the margin, must be redrawn. */
		uint32_t fg = pack(colors.foreground), bg = pack(colors.background);
		uint32_t cc = colors.cursor_has_value ? pack(colors.cursor) : t->theme.cursor;
		if (!t->colors_known || fg != t->last_fg || bg != t->last_bg || cc != t->cur_color) {
			t->colors_known = 1;
			t->last_fg = fg;
			t->last_bg = bg;
			t->cur_color = cc;
			r->padbg = bg;
			t->full = 1;
		}
	}
	ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cur);
	t->cur_in_view = cur.viewport_has_value;
	t->cur_x = cur.viewport_x;
	t->cur_y = cur.viewport_y;
	t->cur_tail = cur.wide_tail;
	t->cur_visible = cur.visible;
	t->cur_blink = cur.blinking;
	t->cur_style = cur.visual_style;
	if (t->cur_x >= t->cols) t->cur_x = t->cols - 1;
	if (t->cur_y >= t->rows) t->cur_y = t->rows - 1;
	int kind = cursor_kind(t);
	if (kind != t->drawn_kind || t->cur_y != t->drawn_y || t->cur_x != t->drawn_x) {
		if (t->drawn_y >= 0 && t->drawn_y < t->rows)
			t->want[t->drawn_y] = 1;
		if (kind != CUR_NONE)
			t->want[t->cur_y] = 1;
		t->drawn_kind = kind;
		t->drawn_x = t->cur_x;
		t->drawn_y = kind != CUR_NONE ? t->cur_y : -1;
	}

	r->begin(r);
	if (t->full)
		r->clear(r, t->cols, t->rows);
	if (ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &t->rit) == GHOSTTY_SUCCESS) {
		while (y < t->rows && ghostty_render_state_row_iterator_next(t->rit)) {
			bool row_dirty = false;
			ghostty_render_state_row_get(t->rit, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty);
			if (!t->full && !row_dirty && !t->want[y]) {
				y++;
				continue;
			}
			RomeCell *prev = t->prev + (size_t)y * t->cols;
			int x0 = 0, x1 = t->cols;
			build_row(t, &colors);
			if (kind != CUR_NONE && y == t->cur_y)
				apply_cursor(t, kind);
			if (!t->full) {
				while (x0 < t->cols && cell_eq(&t->rowbuf[x0], &prev[x0]))
					x0++;
				while (x1 > x0 && cell_eq(&t->rowbuf[x1 - 1], &prev[x1 - 1]))
					x1--;
			}
			if (x1 > x0) {
				if (x0 > 0 && t->rowbuf[x0].width == 0)
					x0--;
				if (x1 < t->cols && x1 > 0 && t->rowbuf[x1 - 1].width == 2)
					x1++;
				r->draw_row(r, y, x0, x1, t->rowbuf);
				drawn++;
				memcpy(prev, t->rowbuf, (size_t)t->cols * sizeof(RomeCell));
				if (!t->full) {
					RomeRect b = { r->padx + x0 * f->cell_w, r->pady + y * f->cell_h, (x1 - x0) * f->cell_w, f->cell_h };
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
			}
			t->want[y] = 0;
			y++;
		}
	}
	if (t->full) {
		nrects = 1;
		rects[0] = (RomeRect){ 0, 0, r->width, r->height };
	} else if (t->has_expose) {
		rects[nrects++] = (RomeRect){ t->ex0, t->ey0, t->ex1 - t->ex0, t->ey1 - t->ey0 };
	}
	t->full = 0;
	t->hint = 0;
	t->has_expose = 0;
	memset(t->want, 0, t->rows);
	ghostty_render_state_clean(t->rs);
	r->present(r, rects, nrects);
	if (r->want_full_redraw) {      /* the renderer lost its image mid-frame (GL atlas reset): repair */
		t->full = 1;
		t->hint = 1;
	}
	return drawn;
}
